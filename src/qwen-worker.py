"""omareader's Qwen3-TTS worker.

omareader starts this with the Python of the environment `omareader setup
--engine qwen` built, keeps it running while there is speech to make, and
talks to it in JSON lines: requests on stdin, replies on stdout, logs on
stderr. The models load once, from local folders only; nothing here touches
the network.

Each request names a batch of sentences. The worker speaks them with the
CustomVoice model, finds each word's start and end with the forced aligner,
and writes one WAV (32-bit float, mono) and one timing TSV per sentence in the
same layout Kokoros uses, then replies with what it wrote.

omareader may cancel a request it no longer needs (the reader moved on, or
the voice changed). A cancelled request still gets its reply, with every
sentence marked "cancelled", but the worker spends as little time on it as it
can: one that has not started is skipped, and one being spoken stops at the
next step of the model.
"""

import argparse
import json
import os
import queue
import sys
import threading
import time

# Belt and braces: the models are local folders, so nothing should ever ask
# the Hugging Face hub for anything, and if something tried it must fail.
os.environ["HF_HUB_OFFLINE"] = "1"
os.environ["TRANSFORMERS_OFFLINE"] = "1"
os.environ["HF_DATASETS_OFFLINE"] = "1"
os.environ["HF_HUB_DISABLE_TELEMETRY"] = "1"
os.environ.setdefault("TOKENIZERS_PARALLELISM", "false")

# Replies go to a private copy of stdout, and stdout itself (the file
# descriptor, which C libraries and child processes such as a missing sox
# write to) is pointed at stderr, so nothing else can corrupt the protocol.
PROTOCOL = os.fdopen(os.dup(1), "w", encoding="utf-8")
os.dup2(2, 1)
sys.stdout = sys.stderr


REPLY_LOCK = threading.Lock()

# Request ids omareader has cancelled. Written by the stdin reader, read by
# the speaking and timing threads; a set behind a lock is enough.
CANCELLED = set()
CANCELLED_LOCK = threading.Lock()
CANCELLED_ERROR = "cancelled"


def is_cancelled(req):
    with CANCELLED_LOCK:
        return req in CANCELLED


def forget(req):
    with CANCELLED_LOCK:
        CANCELLED.discard(req)


def reply(message):
    with REPLY_LOCK:
        PROTOCOL.write(json.dumps(message, ensure_ascii=False) + "\n")
        PROTOCOL.flush()


def log(message):
    sys.stderr.write(message + "\n")
    sys.stderr.flush()


# Generation settings omareader may pass, with the type each must have. Any
# other key is ignored, so a newer omareader never crashes an older worker.
GENERATION_KEYS = {
    "temperature": float,
    "top_p": float,
    "top_k": int,
    "repetition_penalty": float,
    "subtalker_temperature": float,
    "subtalker_top_p": float,
    "subtalker_top_k": int,
}

# Free GPU memory the forced aligner may take while leaving room for the
# speech model's working memory.
ALIGNER_GPU_BYTES = 2_500_000_000

# The 12 Hz codec makes about 12.5 frames per second of speech.
CODEC_FRAMES_PER_SECOND = 12.5


def token_budget(text):
    # Generous for the slowest reading of the sentence, but a bound: a model
    # that loses its place would otherwise talk until max_new_tokens.
    seconds = 4.0 + 0.2 * len(text)
    return int(min(2048, CODEC_FRAMES_PER_SECOND * seconds))


def aligner_language(language, text):
    # The aligner only changes how it splits text into words: Japanese and
    # Korean have their own tokenizers, everything else splits on spaces
    # (and per character for Chinese).
    if language and language.lower() != "auto":
        return language
    for ch in text:
        code = ord(ch)
        if 0x3040 <= code <= 0x30FF:
            return "Japanese"
        if 0xAC00 <= code <= 0xD7AF:
            return "Korean"
    return "English"


def write_tsv(path, items):
    with open(path, "w", encoding="utf-8") as f:
        f.write("word\tstart_sec\tend_sec\n")
        for item in items:
            word = item.text.replace("\t", " ").replace("\n", " ")
            f.write(f"{word}\t{item.start_time:.3f}\t{item.end_time:.3f}\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True)
    parser.add_argument("--aligner", required=True)
    parser.add_argument("--device", default="auto", choices=["auto", "cuda", "cpu"])
    # The free GPU memory the model needs; with less, "auto" uses the CPU
    # rather than running out of memory partway through a chapter.
    parser.add_argument("--gpu-mib", type=int, default=0)
    args = parser.parse_args()

    started = time.monotonic()
    try:
        import numpy as np
        import soundfile as sf
        import torch
        from qwen_asr import Qwen3ForcedAligner
        from qwen_tts import Qwen3TTSModel
    except Exception as error:  # a broken environment
        reply({"event": "fatal", "message": f"The Qwen3-TTS environment is incomplete: {error}"})
        return 1

    if args.device == "cuda" and not torch.cuda.is_available():
        reply({"event": "fatal", "message": "No CUDA GPU is available to PyTorch."})
        return 1
    device = "cuda:0" if args.device != "cpu" and torch.cuda.is_available() else "cpu"
    if device != "cpu" and args.device == "auto" and args.gpu_mib > 0:
        free, _ = torch.cuda.mem_get_info()
        if free < args.gpu_mib * 1024 * 1024:
            log(f"{free // 1048576} MiB free on the GPU, {args.gpu_mib} needed; using the CPU")
            device = "cpu"
    # bfloat16 halves the memory on a GPU; CPUs are faster in float32.
    dtype = torch.bfloat16 if device.startswith("cuda") else torch.float32
    if device == "cpu":
        torch.set_num_threads(max(1, (os.cpu_count() or 2) - 1))
    else:
        # Only the aligner computes on the CPU; leave the rest of it free.
        torch.set_num_threads(max(1, (os.cpu_count() or 2) // 2))

    try:
        tts = Qwen3TTSModel.from_pretrained(args.model, device_map=device, dtype=dtype, local_files_only=True)
        # The aligner needs about 1.5 GB. On a small GPU shared with the
        # desktop it goes to the CPU instead, where one pass per sentence
        # still takes a fraction of a second.
        aligner_device, aligner_dtype = device, dtype
        if device.startswith("cuda"):
            free, _ = torch.cuda.mem_get_info()
            if free < ALIGNER_GPU_BYTES:
                aligner_device, aligner_dtype = "cpu", torch.float32
        aligner = Qwen3ForcedAligner.from_pretrained(args.aligner, device_map=aligner_device, dtype=aligner_dtype,
                                                     local_files_only=True)
    except Exception as error:
        message = str(error)
        if "out of memory" in message.lower():
            message = "The GPU does not have enough free memory for Qwen3-TTS: " + message
        reply({"event": "fatal", "message": message[:600]})
        return 1

    speakers = tts.get_supported_speakers() or []
    languages = tts.get_supported_languages() or []
    gpu = torch.cuda.get_device_name(0) if device.startswith("cuda") else ""
    reply({
        "event": "ready",
        "device": device,
        "gpu": gpu,
        "alignerDevice": aligner_device,
        "speakers": speakers,
        "languages": languages,
        "instruct": tts.model.tts_model_size not in "0b6",
        "loadSeconds": round(time.monotonic() - started, 2),
    })

    # A request being spoken stops early once cancelled. qwen_tts does not
    # pass stopping criteria through, so they are added where it calls the
    # talker's generate().
    current = {"req": None}
    install_cancel_check(tts, current, torch)

    # Alignment runs on its own thread, so while the aligner times one batch
    # (often on the CPU) the GPU is already speaking the next. Stdin is read
    # on another, so a cancel arrives while a batch is being spoken.
    pending = queue.Queue(maxsize=2)
    work = queue.Queue()
    finisher = threading.Thread(target=finish_loop, args=(pending, aligner, sf), daemon=True)
    finisher.start()
    threading.Thread(target=read_loop, args=(work,), daemon=True).start()
    while True:
        request = work.get()
        if request is None:
            break
        req = request.get("req")
        if is_cancelled(req):
            reply(cancelled_reply(request))
            forget(req)
            continue
        current["req"] = req
        try:
            job = generate(request, tts, np, torch)
        finally:
            current["req"] = None
        pending.put(job)
    pending.put(None)
    finisher.join()
    return 0


def read_loop(work):
    """Reads requests from omareader; quitting or a closed stdin ends the worker."""
    for raw in sys.stdin:
        raw = raw.strip()
        if not raw:
            continue
        try:
            request = json.loads(raw)
        except ValueError:
            log("ignored a line that is not JSON")
            continue
        op = request.get("op")
        if op == "quit":
            break
        if op == "cancel":
            with CANCELLED_LOCK:
                CANCELLED.add(request.get("req"))
        elif op == "synth":
            work.put(request)
    work.put(None)


def install_cancel_check(tts, current, torch):
    try:
        from transformers import StoppingCriteria, StoppingCriteriaList
        talker = tts.model.talker
        original = talker.generate
    except Exception as error:  # another qwen_tts layout; cancelling then waits for the batch
        log(f"cannot stop a batch early: {error}")
        return

    class Cancelled(StoppingCriteria):
        def __call__(self, input_ids, scores, **kwargs):
            stop = is_cancelled(current["req"])
            return torch.full((input_ids.shape[0],), stop, dtype=torch.bool, device=input_ids.device)

    def generate(*args, **kwargs):
        criteria = kwargs.pop("stopping_criteria", None) or StoppingCriteriaList()
        criteria.append(Cancelled())
        return original(*args, stopping_criteria=criteria, **kwargs)

    talker.generate = generate


def cancelled_reply(request):
    items = request.get("items") or []
    return {"event": "done", "req": request.get("req"), "items": [{"ok": False, "error": CANCELLED_ERROR}] * len(items),
            "seconds": 0}


def generate(request, tts, np, torch):
    """Speaks a request's sentences; returns them for finish() to align."""
    started = time.monotonic()
    items = request.get("items") or []
    speaker = request.get("speaker") or "Ryan"
    language = request.get("language") or "Auto"
    instruct = request.get("instruct") or ""
    generation = {}
    for key, kind in GENERATION_KEYS.items():
        value = (request.get("generation") or {}).get(key)
        if value is not None:
            generation[key] = kind(value)
    seed = int(request.get("seed", 1234))
    texts = [item.get("text", "") for item in items]
    job = {"request": request, "texts": texts, "language": language, "started": started,
           "audios": [None] * len(items), "errors": ["no audio"] * len(items), "rate": 24000}

    def speak(batch, attempt_seed):
        # A fixed seed makes the same sentence with the same settings sound
        # the same in the preview, while reading, and in an export.
        torch.manual_seed(attempt_seed)
        budget = max(token_budget(t) for t in batch)
        wavs, rate = tts.generate_custom_voice(text=batch, speaker=speaker, language=language,
                                               instruct=instruct or None, max_new_tokens=budget, **generation)
        return [np.asarray(w, dtype=np.float32).reshape(-1) for w in wavs], rate

    try:
        audios, rate = speak(texts, seed)
    except torch.cuda.OutOfMemoryError as error:
        # A batch needs more memory than one sentence; when the GPU is shared
        # and nearly full, speaking them one at a time still fits.
        torch.cuda.empty_cache()
        if len(texts) == 1:
            job["errors"] = ["The GPU ran out of memory: " + str(error)[:300]]
            return job
        log(f"out of GPU memory for {len(texts)} sentences; speaking them one at a time")
        audios, rate = [], 24000
        for text in texts:
            if is_cancelled(request.get("req")):
                break
            try:
                one, rate = speak([text], seed)
                audios.append(one[0])
            except Exception as one_error:
                torch.cuda.empty_cache()
                audios.append(np.zeros(0, dtype=np.float32))
                job["errors"][len(audios) - 1] = str(one_error)[:400]
    except Exception as error:
        job["errors"] = [str(error)[:400]] * len(items)
        return job
    job["rate"] = rate
    if is_cancelled(request.get("req")):
        return job
    for i, samples in enumerate(audios):
        # A sentence that ran into its token budget lost its place and kept
        # talking; one more try with another seed almost always recovers.
        if samples.size >= (token_budget(texts[i]) - 2) * rate / CODEC_FRAMES_PER_SECOND \
                and not is_cancelled(request.get("req")):
            log(f"sentence {i} ran over its budget; trying again")
            try:
                retry, _ = speak([texts[i]], seed + 1)
                samples = retry[0]
            except Exception as error:
                job["errors"][i] = str(error)[:400]
                continue
        if samples.size > 0:
            job["audios"][i] = samples
    return job


def finish_loop(pending, aligner, sf):
    while True:
        job = pending.get()
        if job is None:
            return
        req = job["request"].get("req")
        reply(cancelled_reply(job["request"]) if is_cancelled(req) else finish(job, aligner, sf))
        forget(req)


def finish(job, aligner, sf):
    """Times each word, writes the WAVs and TSVs, and builds the reply."""
    request, texts, audios, rate = job["request"], job["texts"], job["audios"], job["rate"]
    items = request.get("items") or []
    results = [{"ok": False, "error": job["errors"][i]} for i in range(len(items))]
    spoken = [i for i, samples in enumerate(audios) if samples is not None]
    alignments = {}
    if spoken:
        try:
            out = aligner.align(audio=[(audios[i], rate) for i in spoken], text=[texts[i] for i in spoken],
                                language=[aligner_language(job["language"], texts[i]) for i in spoken])
            for i, result in zip(spoken, out):
                alignments[i] = list(result)
        except Exception as error:
            # The sentence still plays without them; omareader then spreads
            # its words over the audio.
            log(f"alignment failed: {error}")
    for i in spoken:
        try:
            sf.write(items[i]["wav"], audios[i], rate, subtype="FLOAT")
            write_tsv(items[i]["tsv"], alignments.get(i, []))
            results[i] = {"ok": True, "seconds": round(audios[i].size / rate, 3), "aligned": i in alignments}
        except Exception as error:
            results[i] = {"ok": False, "error": str(error)[:400]}
    return {"event": "done", "req": request.get("req"), "items": results,
            "seconds": round(time.monotonic() - job["started"], 3)}


if __name__ == "__main__":
    sys.exit(main())
