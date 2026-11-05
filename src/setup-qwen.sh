#!/bin/sh
# Installs Qwen3-TTS for omareader: a private Python environment with pinned,
# hash-checked packages, the CustomVoice speech model (with its speech
# tokenizer) and the forced aligner that times each spoken word. This and
# `omareader setup` for Kokoro are the only steps that use the network;
# afterwards omareader reads and exports fully offline.
#
# Safe to run again: anything already verified is left alone, and a run that
# stopped halfway picks up where it left off. Nothing counts as installed
# until it has been checked, so a failed run never leaves a half-working voice.
#
#   --model 0.6b   the default, for GPUs with about 4 GB free (or the CPU)
#   --model 1.7b   steadier and takes a style instruction; about 7 GB of GPU memory
#   --dry-run      show what would be done without changing anything
set -eu

DRY_RUN=0
MODEL_SIZE=0.6b
while [ $# -gt 0 ]; do
  case "$1" in
    --dry-run) DRY_RUN=1 ;;
    --model)
      [ $# -ge 2 ] || { echo "--model needs 0.6b or 1.7b" >&2; exit 2; }
      MODEL_SIZE="$2"
      shift
      ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done

case "$MODEL_SIZE" in
  0.6b)
    MODEL_NAME="Qwen3-TTS-12Hz-0.6B-CustomVoice"
    MODEL_REV="85e237c12c027371202489a0ec509ded67b5e4b5"
    MODEL_GB=3
    NEED_MIB=4000
    ;;
  1.7b)
    MODEL_NAME="Qwen3-TTS-12Hz-1.7B-CustomVoice"
    MODEL_REV="0c0e3051f131929182e2c023b9537f8b1c68adfe"
    MODEL_GB=5
    NEED_MIB=7000
    ;;
  *) echo "The model is 0.6b or 1.7b." >&2; exit 2 ;;
esac
ALIGNER_NAME="Qwen3-ForcedAligner-0.6B"
ALIGNER_REV="c7cbfc2048c462b0d63a45797104fc9db3ad62b7"
ALIGNER_GB=2
# The environment with PyTorch and its CUDA libraries, plus uv's download cache.
RUNTIME_GB=14
PYTHON_VERSION=3.12

DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
RUNTIME="$DATA_HOME/omareader/qwen"
VENV="$RUNTIME/venv"
MODEL_DIR="${OMAREADER_MODELS:-$DATA_HOME/omareader/models}/qwen"
MARKER=".omareader-verified"

say() { printf '%s\n' "$*"; }
run() {
  if [ "$DRY_RUN" = 1 ]; then
    say "would run: $*"
  else
    "$@"
  fi
}

# The Python packages, pinned with uv pip compile --generate-hashes for
# qwen-tts 0.1.1 and qwen-asr 0.0.6 (transformers overridden to 4.57.6, which
# both work with), keeping the hashes for Python 3.12 on x86_64 Linux.
requirements() {
  cat <<'LOCK'
accelerate==1.12.0 --hash=sha256:3e2091cd341423207e2f084a6654b1efcd250dc326f2a37d6dde446e07cabb11
annotated-doc==0.0.5 --hash=sha256:117bac03a25ede5df5440e855b32d556049ca169ead221505badf432fed4b101
annotated-types==0.8.0 --hash=sha256:f072f4d804ea359e4eaf198b1af7a8b0943881a87f31bb764f8bf219bb9419e0
anyio==4.16.0 --hash=sha256:c66ae634e0b8228396b801741beb3294326d4b6102f7bba4041c0dfb58c29026
av==19.0.2 --hash=sha256:9c1da7aad0fa6c13b4a5c1e7985be0d13f8526a96284573acaf3d55ead6d3fe8
blinker==1.9.0 --hash=sha256:ba0efaa9080b619ff2f3459d1d500c57bddea4a6b424b60a91141db6fd2f08bc
brotli==1.2.1 --hash=sha256:4b9dd0173ca5e01d1eb307d3c184fe8c09bdc168ede560889b1018d216d38c61
certifi==2026.10.23 --hash=sha256:e9b54de76e530bb6cf0ccd036787103acb58fd42ba930ea8bf948ddbaa2f61d1
cffi==2.1.1 --hash=sha256:c1453022f490d2459a11819d83ad1d586e9ff65a12ac3e705ffebd46d3685dcf
charset-normalizer==3.5.2 --hash=sha256:3d31298449090ab8d47b7b1b2a555ff73cac7ed438a08b7ac160980c7ebed649 --hash=sha256:b6b751274acb69d77b3323d6b7dbaa3c7fdfc1eb829b7eb61d262f32e1af9685
click==8.5.0 --hash=sha256:255bc9599cf7748b4b1a446ccc735421bd08a2ae529a8b88597d3de5664ee360
cloudpickle==3.1.2 --hash=sha256:9acb47f6afd73f60dc1df93bb801b472f05ff42fa6c84167d25cb206be1fbf4a
cuda-bindings==13.4.4 --hash=sha256:433efa31e33c2868621b5f6036bd3bf5de9c45c5546cb1361a5f6ff3415b8bb5
cuda-pathfinder==1.8.5 --hash=sha256:16d1026fdd78ac0222b966441de53dec3f0dbe6b1e2c43d19d92eb29bfd81e67
cuda-toolkit==13.2.2 --hash=sha256:d12edc1aad1356236a2ee10c0d794b1aff5d3e79bfeca54867c3bb8e5afd87b0
cython==3.3.1 --hash=sha256:215669fd0ce792080cd4d0748db7e1e31f424b6706a6220d08339895e6a3a03a --hash=sha256:61abc7feedd20dac6c4ff7ab3a30e17136a067b242ab9253e2b4a6d9a72c2b61
decorator==5.3.1 --hash=sha256:f47fe6fdbd2edd623ecfe36875d37aba411624e2670dd395dddae1358689bb3c
dynet38==2.2 --hash=sha256:8693bf8e98fdddfa116fa1e213f1c471be772e1b9e006ad5278a2b2a8f7b4d70
einops==0.8.2 --hash=sha256:54058201ac7087911181bfec4af6091bb59380360f069276601256a76af08193
fastapi==0.162.0 --hash=sha256:3785963de305dd09912426cb81f2d230fce98ab3521cdfe97265c8d14983128c
filelock==4.2.9 --hash=sha256:a379c50a729e5b68c8519a4d26489f755d9769960a236ecd838190e2a78db2e9
flask==3.1.3 --hash=sha256:f4bcbefc124291925f1a26446da31a5178f9483862233b23c0c96a20701f670c
flatbuffers==25.12.19 --hash=sha256:7634f50c427838bb021c2d66a3d1168e9d199b0607e6329399f04846d42e20b4
fsspec==2026.9.0 --hash=sha256:8dd6e646e99ea382bd85f97a45e6b526a442d79423a7dc673f1e2756d05fcb5f
gradio==6.17.3 --hash=sha256:7e52c65bfbb7bd75ac1c28cb38f93b01e5f6a2ff013224e6213533451bfee517
gradio-client==2.5.0 --hash=sha256:d43e2179c29076292a76485ad7ed2e6eaa19d14ac58283bd7f5beabfe4ca958c
groovy==0.1.2 --hash=sha256:7f7975bab18c729a257a8b1ae9dcd70b7cafb1720481beae47719af57c35fa64
h11==0.16.0 --hash=sha256:63cf8bbe7522de3bf65932fda1d9c2772064ffb3dae62d55932da54b31cb6c86
hf-gradio==0.4.1 --hash=sha256:76b8cb8be6abe62d74c1ad2d35b42f0629db89aa9e1a8d033cecfe7c856eeab3
hf-xet==1.8.0 --hash=sha256:10ed23b51dacf7917d64f335c25048deb470efa755e9989b218fc0531386138e
httpcore==1.0.9 --hash=sha256:2d400746a40668fc9dec9810239072b40b4484b640a8c38fd654a024c7a1bf55
httpx==0.28.1 --hash=sha256:d909fcccc110f8c7faf814ca82a9a4d816bc5a6dbfea25d6591d6985b8ba59ad
huggingface-hub==0.36.2 --hash=sha256:48f0c8eac16145dfce371e9d2d7772854a4f591bcb56c9cf548accf531d54270
idna==3.20 --hash=sha256:ab7ae7122974553370f0bdb919e1a960b2cd1bc1ef0276416d896db81c14582c
itsdangerous==2.2.0 --hash=sha256:c6242fc49e35958c8b15141343aa660db5fc54d4f13a1db01a3f5891b98700ef
jinja2==3.1.6 --hash=sha256:85ece4451f492d0c13c5dd7c13a64681a86afae63a5f347908daf103ce6d2f67
joblib==1.6.0 --hash=sha256:3dbbf9f6e4b592a2357b854608e980fe6390d131d7a82f011a377ef2ebef7aba
lazy-loader==0.6 --hash=sha256:77253be3391b06124a0e16105bd663b6c54470af1a9ca8e1cf026f38d58ed056
librosa==1.0.0 --hash=sha256:5910a6c0e1b2e494b92758c1615a7acbd0515a2315e138927ba2982f2af88857
llvmlite==0.50.0 --hash=sha256:d501e5103076b9a14be885d2574dc2f6793171aa54a853d1244e011d476f1399
markdown-it-py==4.2.0 --hash=sha256:9f7ebbcd14fe59494226453aed97c1070d83f8d24b6fc3a3bcf9a38092641c4a
markupsafe==3.0.4 --hash=sha256:8e124f974786f831d6043728e38296969d3579db8896fe004682f5758e613581
mdurl==0.1.2 --hash=sha256:84008a41e51615a49fc9966191ff91509e3c40b939176e643fd50a5c2196b8f8
mpmath==1.3.0 --hash=sha256:a0b2b9fe80bbcd81a6647ff13108738cfb482d481d826cc0e02f5b35e5c88d2c
msgpack==1.2.4 --hash=sha256:c4d5bbdbea8f5669b3a734d6c226772804fb120848f99cbe63a0ddee63b049c9
nagisa==0.2.11 --hash=sha256:43f2a5a30aa4b5c3f9018062e4c892efb6c18ae80f0d67a9a0611aa3d260dc37
narwhals==2.28.0 --hash=sha256:1f69d6385ec30e2fa6e1fdeb37c562ecabd0f713f6f3e3b58a668c5c9df9567f
networkx==3.7 --hash=sha256:e3fd2c13a7814cee3746340d8d7f8598a67f16a58bf47fb7f8793fab6efca1b0
numba==0.68.1 --hash=sha256:8b33fee239194b9b6cb9b176f3c2563b94e5882fd846748ad0cf960daeee5406
numpy==2.5.4 --hash=sha256:fbde6962867ee75b48b0ee29b2b9372ec5d617799dbaf38e82dc0596f2f7738a
nvidia-cublas==13.4.1.3 --hash=sha256:a5917cb642cfb07f4a0231ee16aec7e2e0357d8cb8b1b78a0c9c68dba3e6afc7
nvidia-cuda-cccl==13.3.5.0.6 --hash=sha256:d5230098fa2df5021e1a00c0c4eee41797671a91119d31e892463e5ccf3ee326
nvidia-cuda-cupti==13.2.86 --hash=sha256:8fa29f15dd8336181f961764d9d95236fc6b931dd0c8fcf8cd70aab0c310b586
nvidia-cuda-nvrtc==13.2.86 --hash=sha256:8111c5130e967b78222ea2124145f8efe9ce201b752b2579a2513100a53be1c5
nvidia-cuda-runtime==13.2.86 --hash=sha256:3498247680dfddce31bad102af7e3e26fc795342a3cac64722bb0fa95e7f60f0
nvidia-cudnn-cu13==9.26.0.51 --hash=sha256:9c976d539786698c71d6bcdbe2053b7485fae062ff7e1215d2f2ff67b1a2149a
nvidia-cufft==12.2.0.57 --hash=sha256:34da27fadf3979b0b042c43823f0530be8408b86df31ee0108e1ca0b621d4ef6
nvidia-cufile==1.17.1.22 --hash=sha256:87feaf8c3ad20d3d03a01933181801ff6892bace5c8f0b1e4b48f319d3e62d36
nvidia-cusolver==12.2.0.11 --hash=sha256:fab5778f6769947389a1a6abc07e635576825a08c0ffc68c3ae7f1392c794b42
nvidia-cusparse==12.7.10.12 --hash=sha256:25907c75e4db1581685f06f61cc6cdecc04611765516a52373e0a18ebd69fa00
nvidia-cusparselt-cu13==0.8.1 --hash=sha256:786ce87568c303fadb5afcc7102d454cd3040d75f6f8626f5db460d1871f4dd0
nvidia-nccl-cu13==2.30.7 --hash=sha256:cefa7fdb9710efd0f39c5f1be1d61ff6fc9a996c451265bd7fbdcf9455ed4b50
nvidia-nvjitlink==13.5.37 --hash=sha256:280d7cb9649704145f074b8dbde500de04ce02d0872e066e20a83c17bcaa93eb
nvidia-nvshmem-cu13==3.7.2 --hash=sha256:b9a4f843c62b5dded8ad33fd6d220e007dcd39f06cee549292f5ba14ce4c235c
nvidia-nvtx==13.2.86 --hash=sha256:968ce72baee2a01791c7d3254000eac81f24677b0b91c62d21b449a1f309c245
onnxruntime==1.31.1 --hash=sha256:1f3dea47f2f0e5117c2404cea7d21fd253ae7836a3bf167a501cdfbb2600d536
opentelemetry-api==1.45.2 --hash=sha256:2cbd737377e710ee740d261c94e93fa0f08e6d037c18f17513cc0a2d94479a34
orjson==3.13.0 --hash=sha256:bceadfd314bd238f584fc229a4bbaf0e573597e7a026dec5429fbf29fd66c641
packaging==26.3 --hash=sha256:d7193f7c8e4e93f444fde0262bf90af30e16fa0ad0ad44cb553c87339b23cd1c
pandas==3.1.1 --hash=sha256:028490c8ac2e03e18c9b08b7f3612e149b03570209bcf741d23833a7a5377ab2
pillow==12.3.0 --hash=sha256:78cb2c6865a35ab8ff8b75fd122f6033b92a62c82801110e48ddd6c936a45d91
platformdirs==4.14.5 --hash=sha256:8e778c6fa421516f4d3369449f8df3b0e1b72c0e0005fe88e3ec772d0a14e750
pooch==1.9.0 --hash=sha256:f265597baa9f760d25ceb29d0beb8186c243d6607b0f60b83ecf14078dbc703b
protobuf==7.36.2 --hash=sha256:bdb3a345d48db958e6ce1f18e508beb0cc981d64f24088427549c866cd039f1e
psutil==7.2.2 --hash=sha256:0746f5f8d406af344fd547f1c8daa5f5c33dbc293bb8d6a16d80b4bb88f59372
pycparser==3.12 --hash=sha256:a076f3f489b39b43ed13563a1dafe42447445bec2e294154ec5c57fcdda4c85e
pydantic==2.14.2 --hash=sha256:7efbf7507b937931cd32a5755acb00ea1cf5ccb1d4e8067a5850d5069e382118
pydantic-core==2.50.2 --hash=sha256:9667637665acba166a7dbbc786f14c50512d3a01f7199e0e1a13b929e92d7c7a
pydub==0.25.1 --hash=sha256:65617e33033874b59d87db603aa1ed450633288aefead953b30bded59cb599a6
pygments==2.21.0 --hash=sha256:2363c69b61c4a97c838da3b130dcd6468f4848992b21a82f2a63ec34377137d9
python-dateutil==4.0.0 --hash=sha256:a5e246f79eb80475e691bf93862cead98decd8a498cf53ba0516fcdc0f50dc1c
python-multipart==0.0.33 --hash=sha256:8f16c07b36add9da6b2ee87d3efcaf0a45a6de9c3d0a5dd94e9ccf7675a7bc45
pytz==2026.6 --hash=sha256:7fed3fee8fd6bd4a2b7b75a8d56021d0624127bf69bd5691f115db929ac570b3
pyyaml==6.0.3 --hash=sha256:ba1cc08a7ccde2d2ec775841541641e4548226580ab850948cbfda66a1befcdc
qwen-asr==0.0.6 --hash=sha256:b9c55a38413298f3a990a4475467399daec6e8f4172363053fc42e2166c2dfd3
qwen-omni-utils==0.0.9 --hash=sha256:f111db07af669c83333411c5177131e18e831fe666d6a55a1af263952ada8939
qwen-tts==0.1.1 --hash=sha256:11a290d8dabc7ef91a90c54478c8ab19b3edb1d85c0882313721892bdc4af15d
regex==2026.9.29 --hash=sha256:39ab5894d971f9ac68baa6eca5c50387db579cfcacf36ae8df3feceb1815e6d0
requests==2.34.2 --hash=sha256:2a0d60c172f83ac6ab31e4554906c0f3b3588d37b5cb939b1c061f4907e278e0
rich==15.0.0 --hash=sha256:33bd4ef74232fb73fe9279a257718407f169c09b78a87ad3d296f548e27de0bb
safehttpx==0.1.7 --hash=sha256:c4f4a162db6993464d7ca3d7cc4af0ffc6515a606dfd220b9f82c6945d869cde
safetensors==0.8.0 --hash=sha256:fabaf3e0f18a6618d9b36560682562157f77c2b71fcffc7b432be2baed9d753d
scikit-learn==1.9.1 --hash=sha256:e5d7b18a5b9dca241a74695f3275fa4c895a9dadc72b3d8df5fa9d1083c9b83e
scipy==1.18.1 --hash=sha256:f55fa87b6c612ecd6b058f167c53231b1d14e412efe361d3d6e38b3631c73218
semantic-version==2.10.0 --hash=sha256:de78a3b8e0feda74cabc54aab2da702113e33ac9d9eb9d2389bcf1f58b7d9177
setuptools==84.2.1 --hash=sha256:ca140b45d5325a9478f880e351484f62fef15f336462e449ca0c03c873d589c2
shellingham==1.5.4 --hash=sha256:7ecfff8f2fd72616f7481040475a65b2bf8af90a56c89140852d1120324e8686
six==1.17.0 --hash=sha256:4721f391ed90541fddacab5acf947aa0d3dc7d27b2e1e8eda2be8970586c3274
soundfile==0.14.0 --hash=sha256:1e38bac1853412871318e82a1ba69a8be677619b56025bbfcccdb41b6cafe82d --hash=sha256:8ba81ae3a89fd5ab3bef8a8eb481fbbe794e806309675a89b4df48b8d31908a8
sox==1.5.0 --hash=sha256:12c7be5bb1f548d891fe11e82c08cf5f1a1d74e225298f60082e5aeb2469ada0
soxr==1.1.0 --hash=sha256:3b033078e86f3c4a658e5697fac8995764fad9e799563616b630136b613167f1
soynlp==0.0.493 --hash=sha256:2aed0ced1f0f74f7bdd0bdc24a979c5cb9ee4a28393642db52a83d174ed65b7a
starlette==1.10.0 --hash=sha256:478c0ab3b4ced6e82719849c118fa406f0278a3499c1a898cd15cd31bdc38ce4
sympy==1.14.0 --hash=sha256:e091cc3e99d2141a0ba2847328f5479b05d94a6635cb96148ccb3f34671bd8f5
threadpoolctl==3.7.0 --hash=sha256:cd8b60b5641b45c67bbf73c64c843235fc2d8a480c87389f52f5dbee893b86be
tokenizers==0.22.2 --hash=sha256:473b83b915e547aa366d1eee11806deaf419e17be16310ac0a14077f1e28f917
tomlkit==0.14.0 --hash=sha256:592064ed85b40fa213469f81ac584f67a4f2992509a7c3ea2d632208623a3680
torch==2.15.0 --hash=sha256:6d8c86f4e33a9f12a58149bd9b0d64351632b8eb44479be8ff85e5389932dd79
torchaudio==2.11.1 --hash=sha256:0173b5b51e1368aa88de3cbe4e98508b5e9df4d8d59356c4e1fbc830c96191ef
tqdm==4.70.1 --hash=sha256:c293e525e6fef9c20e8728fd4612df02a0aa31bb5fe91ecd93e123b1b7bffa73
transformers==4.57.6 --hash=sha256:4c9e9de11333ddfe5114bc872c9f370509198acf0b87a832a0ab9458e2bd0550
triton==3.9.0 --hash=sha256:5e52ab4e2f1d64f0ac494c9aa723697b793f81a91c4cae4c36608ca0bc4267bf
typer==0.27.3 --hash=sha256:e50022f28b82a86313e54501317a1db64bf8f8d036ff8cfe5ca7e47675454aff
typing-extensions==4.16.0 --hash=sha256:481caa481374e813c1b176ada14e97f1f67a4539ce9cfeb3f350d78d6370c2e8
typing-inspection==0.4.5 --hash=sha256:bc210535442b39e1bf2805e0c7a17dfd6a4ace760c84c7f7d16245d23a6b1c39
tzdata==2026.6 --hash=sha256:0a10eec6e13ef16b627fabed9a5f2ba61a62b1c67f327305f4e7e412a70b4aec
urllib3==2.8.0 --hash=sha256:0cf3cae568d36aa9576b28dfb35f11328f1cb974ca7647d9475ebb86c75ac6e3
uvicorn==0.54.0 --hash=sha256:505bdb0f318731d45f1f712071fc781a8981f6847a31c902c9f5e652d4f67faf
werkzeug==3.1.9 --hash=sha256:6392e50c78460ba618e5b21f08a71f59c99ce99cdc6cf6e3dd7e6ccca8754fab
LOCK
}

# Every file each model needs, with its sha256, at the pinned revision.
manifest() {
  case "$1" in
    Qwen3-TTS-12Hz-0.6B-CustomVoice)
      cat <<'SUMS'
81aca2b6fac304944d8acf345272d8a9a727d5fc2e2e66b222ab4729340c7455 config.json
f1b90b4513f3b34c62851049e2492d7b4c5940daf1276f89c82b8ef04127f3aa generation_config.json
599bab54075088774b1733fde865d5bd747cbcc7a547c5bc12610e874e26f5e3 merges.txt
bc3c7e785eb961179c25450d1acff03f839e0002f2f3a5aeb67b5735c0fa2adb model.safetensors
efdde1022ea9d76928bf7a9cd53139138f5ba2e466e837f08f6105ab1af1c119 preprocessor_config.json
ee65bb901c876664ab8707c487157aa1a6ee57c65969b28fb5ec9dc211e68167 speech_tokenizer/config.json
6bc26d64eb5024b4d1dab5a52371958b429256d6c9d59787f1f5294a54e0cebd speech_tokenizer/configuration.json
836b7b357f5ea43e889936a3709af68dfe3751881acefe4ecf0dbd30ba571258 speech_tokenizer/model.safetensors
fcb3805e597e786d4067706e602f6688524640f8d3396790e2e09b5942fcbdfb speech_tokenizer/preprocessor_config.json
dc3c31c3bdaedd5016382bb3cbe07323026775ad51f5a4fb564505992ae4a670 tokenizer_config.json
ca10d7e9fb3ed18575dd1e277a2579c16d108e32f27439684afa0e10b1440910 vocab.json
SUMS
      ;;
    Qwen3-TTS-12Hz-1.7B-CustomVoice)
      cat <<'SUMS'
17a07f527a1c25ea30b4e023a184482a23d3e279d697b1dc81b1bde498d29cf9 config.json
f1b90b4513f3b34c62851049e2492d7b4c5940daf1276f89c82b8ef04127f3aa generation_config.json
599bab54075088774b1733fde865d5bd747cbcc7a547c5bc12610e874e26f5e3 merges.txt
38b1d5971bdbd982b561cccec982669a53b0537c3cf5e9bd4778ed07bb2f5137 model.safetensors
efdde1022ea9d76928bf7a9cd53139138f5ba2e466e837f08f6105ab1af1c119 preprocessor_config.json
ee65bb901c876664ab8707c487157aa1a6ee57c65969b28fb5ec9dc211e68167 speech_tokenizer/config.json
6bc26d64eb5024b4d1dab5a52371958b429256d6c9d59787f1f5294a54e0cebd speech_tokenizer/configuration.json
836b7b357f5ea43e889936a3709af68dfe3751881acefe4ecf0dbd30ba571258 speech_tokenizer/model.safetensors
fcb3805e597e786d4067706e602f6688524640f8d3396790e2e09b5942fcbdfb speech_tokenizer/preprocessor_config.json
dc3c31c3bdaedd5016382bb3cbe07323026775ad51f5a4fb564505992ae4a670 tokenizer_config.json
ca10d7e9fb3ed18575dd1e277a2579c16d108e32f27439684afa0e10b1440910 vocab.json
SUMS
      ;;
    Qwen3-ForcedAligner-0.6B)
      cat <<'SUMS'
75a8cfca24f00de72d796fbfed6858fc9614ef3dabd8696684cc3bc03a9c58ff chat_template.json
d616c65d46c4b90bdc651b0a0963ea932732241140f337f9bb6b0335a9c8ef09 config.json
948d089b23bca1d214e768d59c4438365665f52ec6d33678f4062206b3fbbb8c generation_config.json
8831e4f1a044471340f7c0a83d7bd71306a5b867e95fd870f74d0c5308a904d5 merges.txt
47831d0e82f96b20e9034dba01a075ee06436654719f6a68289e49f1b65ce0e7 model.safetensors
45e120a4eda2c20c5d7f2ea9354e63536bf35e27aa573fb7cdf78017b378770d preprocessor_config.json
3ab80063f8511deb9566e6ad438d17b7a6277fcffd52d92854112f19d36bd81c tokenizer_config.json
ca10d7e9fb3ed18575dd1e277a2579c16d108e32f27439684afa0e10b1440910 vocab.json
SUMS
      ;;
  esac
}

missing=""
for tool in uv curl sha256sum df; do
  command -v "$tool" >/dev/null 2>&1 || missing="$missing $tool"
done
if [ -n "$missing" ]; then
  say "Missing:$missing"
  say "Install them first, for example: omarchy-pkg-add uv curl"
  exit 1
fi

if [ "$(uname -m)" != "x86_64" ]; then
  say "Qwen3-TTS is set up for x86_64 Linux only; this machine is $(uname -m)."
  exit 1
fi

# The hardware decides how usable Qwen3-TTS is, not whether it installs.
if command -v nvidia-smi >/dev/null 2>&1 \
  && gpu="$(nvidia-smi --query-gpu=name,memory.total,memory.free --format=csv,noheader,nounits 2>/dev/null | head -n 1)" \
  && [ -n "$gpu" ]; then
  gpu_name="$(printf '%s' "$gpu" | cut -d, -f1 | sed 's/^ *//')"
  gpu_free="$(printf '%s' "$gpu" | cut -d, -f3 | tr -d ' ')"
  say "GPU: $gpu_name, $gpu_free MiB free"
  if [ "$gpu_free" -lt "$NEED_MIB" ] 2>/dev/null; then
    say "Note: the $MODEL_SIZE model wants about $NEED_MIB MiB of free GPU memory to read along in real time."
    say "      With less it falls back to the CPU, which is fine for exports but too slow to listen along."
  fi
else
  say "GPU: no NVIDIA GPU found. Qwen3-TTS will run on the CPU, about a third of real time:"
  say "     fine for exports left running, too slow to listen along. Kokoro reads in real time on any CPU."
fi

requirements_sum="$(requirements | sha256sum | cut -d' ' -f1)"
need_runtime=1
if [ -x "$VENV/bin/python" ] && [ -f "$VENV/$MARKER" ] && [ "$(cat "$VENV/$MARKER")" = "$requirements_sum" ]; then
  need_runtime=0
fi
model_verified() {
  [ -f "$MODEL_DIR/$1/$MARKER" ] && [ "$(cat "$MODEL_DIR/$1/$MARKER")" = "$2" ]
}

# Check there is room before downloading gigabytes.
need_gb=0
[ "$need_runtime" = 1 ] && need_gb=$((need_gb + RUNTIME_GB))
model_verified "$MODEL_NAME" "$MODEL_REV" || need_gb=$((need_gb + MODEL_GB))
model_verified "$ALIGNER_NAME" "$ALIGNER_REV" || need_gb=$((need_gb + ALIGNER_GB))
if [ "$need_gb" -gt 0 ]; then
  probe="$DATA_HOME"
  while [ ! -d "$probe" ]; do probe="$(dirname "$probe")"; done
  free_gb=$(($(df -Pk "$probe" | awk 'NR == 2 { print $4 }') / 1048576))
  if [ "$free_gb" -lt "$need_gb" ]; then
    say "Not enough disk space: this needs about $need_gb GB in $probe and $free_gb GB is free."
    exit 1
  fi
fi

if [ "$need_runtime" = 1 ]; then
  say "Python: building the environment in $VENV (several GB, a few minutes)"
  lock="$(mktemp)"
  trap 'rm -f "$lock"' EXIT
  requirements > "$lock"
  run mkdir -p "$RUNTIME"
  run rm -f "$VENV/$MARKER"
  run uv venv --quiet --allow-existing --python "$PYTHON_VERSION" "$VENV"
  run uv pip sync --quiet --python "$VENV/bin/python" --require-hashes "$lock"
  # The packages must import, or the environment doesn't count. Their
  # import chatter (an optional sox, flash-attn) is left out.
  if [ "$DRY_RUN" = 1 ]; then
    say "would check that the packages import and whether CUDA works"
  else
    if ! output="$(env HF_HUB_OFFLINE=1 TRANSFORMERS_OFFLINE=1 "$VENV/bin/python" -I -c \
      "import qwen_tts, qwen_asr, soundfile, torch; print('CUDA=' + ('available' if torch.cuda.is_available() else 'not available'))" \
      2>&1)"; then
      printf '%s\n' "$output" | tail -n 20
      say "The packages installed but do not import; nothing was marked as installed."
      exit 1
    fi
    say "Python: ready, CUDA $(printf '%s\n' "$output" | sed -n 's/^CUDA=//p' | tail -n 1)"
    printf '%s\n' "$requirements_sum" > "$VENV/$MARKER"
  fi
else
  say "Python: environment already in place ($VENV)"
fi

# Downloads each file the model needs into its folder, keeping any already
# there that match, and marks the folder verified only once all of them do.
fetch_model() {
  name="$1"; rev="$2"; dir="$MODEL_DIR/$name"
  if model_verified "$name" "$rev"; then
    say "Model: $name already in place"
    return
  fi
  say "Model: $name"
  run rm -f "$dir/$MARKER"
  manifest "$name" | while read -r sum path; do
    target="$dir/$path"
    if [ -f "$target" ] && printf '%s  %s\n' "$sum" "$target" | sha256sum -c --status; then
      continue
    fi
    say "  downloading $path"
    run mkdir -p "$(dirname "$target")"
    run curl -fL --retry 3 --progress-bar -o "$target.part" "https://huggingface.co/Qwen/$name/resolve/$rev/$path"
    if [ "$DRY_RUN" = 1 ]; then
      continue
    fi
    if ! printf '%s  %s\n' "$sum" "$target.part" | sha256sum -c --status; then
      rm -f "$target.part"
      say "The download of $name/$path did not match its checksum; it was not installed."
      exit 1
    fi
    mv "$target.part" "$target"
  done || exit 1
  if [ "$DRY_RUN" = 0 ]; then
    printf '%s\n' "$rev" > "$dir/$MARKER"
  fi
}

fetch_model "$MODEL_NAME" "$MODEL_REV"
fetch_model "$ALIGNER_NAME" "$ALIGNER_REV"

if [ "$DRY_RUN" = 1 ]; then
  say "Dry run: nothing was changed."
else
  say "Done. Choose Qwen3-TTS in omareader's voice settings; it reads offline from now on."
  if [ "$MODEL_SIZE" = 1.7b ]; then
    say "Then pick the 1.7B model under Advanced, or pass --tune model=1.7b to export."
  fi
fi
