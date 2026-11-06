package io.github.looizao.omareader

import android.content.Context
import android.util.AttributeSet
import android.view.MotionEvent
import android.widget.TextView

// One paragraph of the chapter. A tap reports the character under the
// finger, so the reader can start reading aloud from the word there; a drag
// still scrolls the list, which takes the gesture over once it moves.
class ParagraphView(context: Context, attrs: AttributeSet?) : TextView(context, attrs) {
    var onTap: ((offset: Int) -> Unit)? = null
    private var downX = 0f
    private var downY = 0f

    init {
        isClickable = true
        setOnClickListener { onTap?.invoke(getOffsetForPosition(downX, downY)) }
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (event.actionMasked == MotionEvent.ACTION_DOWN) {
            downX = event.x
            downY = event.y
        }
        return super.onTouchEvent(event)
    }

    // Accessibility services click without touching; they start at the
    // paragraph's first character.
    override fun performClick(): Boolean = super.performClick()
}
