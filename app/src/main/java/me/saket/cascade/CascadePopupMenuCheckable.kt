@file:SuppressLint("RestrictedApi")
@file:Suppress("DeprecatedCallableAddReplaceWith")

package me.saket.cascade

import android.annotation.SuppressLint
import android.content.Context
import android.util.TypedValue
import android.view.Gravity
import android.view.Menu
import android.view.MenuItem
import android.view.SubMenu
import android.view.View
import android.view.View.SCROLLBARS_INSIDE_OVERLAY
import android.view.ViewGroup
import android.view.ViewGroup.LayoutParams
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import androidx.annotation.MenuRes
import androidx.appcompat.view.SupportMenuInflater
import androidx.appcompat.view.menu.MenuBuilder
import androidx.appcompat.view.menu.MenuItemImpl
import androidx.appcompat.view.menu.SubMenuBuilder
import androidx.appcompat.widget.PopupMenu
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import androidx.recyclerview.widget.RecyclerView.RecycledViewPool
import java.util.Stack
import kotlin.DeprecationLevel.ERROR

// Derived from https://github.com/saket/cascade
open class CascadePopupMenuCheckable @JvmOverloads constructor(
    private val context: Context,
    private val anchor: View,
    private var gravity: Int = Gravity.NO_GRAVITY,
    private val styler: CascadePopupMenu.Styler = CascadePopupMenu.Styler(),
    private val fixedWidth: Int = context.dip(196),
    private val defStyleAttr: Int = android.R.style.Widget_Material_PopupMenu,
    private val backNavigator: CascadeBackNavigator = CascadeBackNavigator()
) {
    val menu: Menu get() = menuBuilder
    val popup = CascadePopupWindow(context, defStyleAttr)

    internal var menuBuilder = MenuBuilder(context)
    private val backstack = Stack<Menu>()
    private val themeAttrs get() = popup.themeAttrs
    private val sharedViewPool = RecycledViewPool()

    private var cascadeAdapter: CascadeMenuAdapter? = null;

    init {
        backNavigator.onBackNavigate = {
            if (backstack.isNotEmpty() && backstack.peek() is SubMenu) {
                val currentMenu = backstack.pop() as SubMenuBuilder
                showMenu(currentMenu.parentMenu as MenuBuilder, goingForward = false)
            }
        }
    }

    fun show() {
        // PopupWindow moves the popup to align with the anchor if a fixed width
        // is known before hand. Note to self: If fixedWidth ever needs to be
        // removed, copy over MenuPopup.measureIndividualMenuWidth().
        popup.width = fixedWidth
        popup.height = WRAP_CONTENT // Doesn't work on API 21 without this.

        popup.setMargins(
            start = context.dip(4),
            end = context.dip(4),
            bottom = context.dip(4)
        )
        styler.background()?.let {
            popup.contentView.background = it
        }

        showMenu(menuBuilder, goingForward = true)
        popup.showAsDropDown(anchor, 0, 0, gravity)
    }

    /**
     * Navigate to the last menu. Also see [CascadeBackNavigator].
     *
     * FYI jumping over multiple back-stack entries isn't supported
     * very well, so avoid navigating multiple menus on a single click.
     */
    fun navigateBack(): Boolean {
        return backNavigator.navigateBack()
    }

    private fun showMenu(menu: MenuBuilder, goingForward: Boolean) {

        cascadeAdapter = CascadeMenuAdapter(
            items = buildModels(menu, canNavigateBack = backstack.isNotEmpty()),
            styler = styler,
            themeAttrs = themeAttrs,
            onTitleClick = { navigateBack() },
            onItemClick = { handleItemClick(it) }
        )
        val menuList = RecyclerView(context).apply {
            layoutManager = LinearLayoutManager(context).also {
                it.recycleChildrenOnDetach = true
                setRecycledViewPool(sharedViewPool)
            }
            isVerticalScrollBarEnabled = true
            scrollBarStyle = SCROLLBARS_INSIDE_OVERLAY
            styler.menuList(this)

            addOnScrollListener(OverScrollIfContentScrolls())
            adapter = cascadeAdapter
            layoutParams = LayoutParams(MATCH_PARENT, WRAP_CONTENT)
        }

        backstack.push(menu)
        popup.contentView.show(menuList, goingForward)
    }

    protected open fun handleItemClick(item: MenuItem) {
        if (item.hasSubMenu()) {
            showMenu(item.subMenu as MenuBuilder, goingForward = true)
            return
        }

        val backstackBefore = backstack.peek()
        (item as MenuItemImpl).invoke()

        if (backstack.peek() === backstackBefore) {
            if (item.isCheckable) {
                cascadeAdapter?.notifyDataSetChanged();
            } else {
                // Back wasn't called. Item click wasn't handled either.
                // Dismiss the popup because there's nothing else to do.
                popup.dismiss()
            }
        }
    }

// === APIs to maintain compatibility with PopupMenu === //

    fun inflate(@MenuRes menuRes: Int) =
        SupportMenuInflater(context).inflate(menuRes, menuBuilder)

    fun setOnMenuItemClickListener(listener: PopupMenu.OnMenuItemClickListener?) =
        menuBuilder.setCallback(listener)

    fun dismiss() =
        popup.dismiss()

    @get:JvmName("getDragToOpenListener")
    @Deprecated("CascadeMenu doesn't support drag-to-open.", level = ERROR)
    val dragToOpenListener: View.OnTouchListener
        get() = error("can't")
}

internal fun MenuBuilder.setCallback(listener: PopupMenu.OnMenuItemClickListener?) {
    setCallback(object : MenuBuilder.Callback {
        override fun onMenuModeChange(menu: MenuBuilder) = Unit
        override fun onMenuItemSelected(menu: MenuBuilder, item: MenuItem): Boolean =
            listener?.onMenuItemClick(item) ?: false
    })
}


internal class CascadeMenuAdapter(
    private val items: List<AdapterModel>,
    private val styler: CascadePopupMenu.Styler,
    private val themeAttrs: CascadePopupWindow.ThemeAttributes,
    private val onTitleClick: (SubMenu) -> Unit,
    private val onItemClick: (MenuItem) -> Unit
) : RecyclerView.Adapter<RecyclerView.ViewHolder>() {

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): RecyclerView.ViewHolder {
        return when (viewType) {
            VIEW_TYPE_HEADER -> MenuHeaderViewHolder.inflate(parent).apply {
                itemView.setBackgroundResource(themeAttrs.touchFeedbackRes)
                itemView.setOnClickListener { onTitleClick(model.menu) }
            }

            VIEW_TYPE_ITEM -> MenuItemViewHolder.inflate(parent).apply {
                itemView.setBackgroundResource(themeAttrs.touchFeedbackRes)
                itemView.setOnClickListener { onItemClick(model.item) }
            }

            else -> TODO()
        }
    }

    override fun onBindViewHolder(holder: RecyclerView.ViewHolder, position: Int) {
        when (holder) {
            is MenuHeaderViewHolder -> {
                holder.render(items[position] as AdapterModel.HeaderModel)
                styler.menuTitle(holder)
            }

            is MenuItemViewHolder -> {
                holder.render(items[position] as AdapterModel.ItemModel)
                styler.menuItem(holder)
            }
        }
    }

    override fun getItemViewType(position: Int): Int {
        return when (items[position]) {
            is AdapterModel.HeaderModel -> VIEW_TYPE_HEADER
            is AdapterModel.ItemModel -> VIEW_TYPE_ITEM
        }
    }

    override fun getItemCount(): Int {
        return items.size
    }

    companion object {
        const val VIEW_TYPE_HEADER = 0
        const val VIEW_TYPE_ITEM = 1
    }
}


internal fun Context.dip(dp: Int): Int {
    val metrics = resources.displayMetrics
    return TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, dp.toFloat(), metrics).toInt()
}

internal class OverScrollIfContentScrolls : RecyclerView.OnScrollListener() {
    override fun onScrollStateChanged(recyclerView: RecyclerView, newState: Int) = Unit
    override fun onScrolled(recyclerView: RecyclerView, dx: Int, dy: Int) {
        if (dy == 0 && dx == 0) {
            // RecyclerView sends 0,0 if the visible item range changes after a layout calculation.
            val canScrollVertical = recyclerView.computeVerticalScrollRange() > recyclerView.height
            recyclerView.overScrollMode =
                if (canScrollVertical) View.OVER_SCROLL_ALWAYS else View.OVER_SCROLL_NEVER
        }
    }
}


@SuppressLint("RestrictedApi")
@OptIn(ExperimentalStdlibApi::class)
internal fun buildModels(menu: MenuBuilder, canNavigateBack: Boolean): List<AdapterModel> {
    val items = mutableListOf<Any>()
    if (menu is SubMenu) items += menu
    items.addAll(menu.nonActionItems.filter { it.isVisible })

    val hasSubMenuItems = items.filterIsInstance<MenuItem>().any { it.hasSubMenu() }

    return items.mapIndexed { index, item ->
        when (item) {
            is SubMenu -> AdapterModel.HeaderModel(
                menu = item,
                showBackIcon = canNavigateBack,
                nextGroupId = (items.getOrNull(index + 1) as? MenuItem)?.groupId
            )

            is MenuItem -> AdapterModel.ItemModel(
                item = item,
                hasSubMenuSiblings = hasSubMenuItems,
                prevGroupId = (items.getOrNull(index - 1) as? MenuItem)?.groupId,
                nextGroupId = (items.getOrNull(index + 1) as? MenuItem)?.groupId
            )

            else -> error("unknown $item")
        }
    }
}