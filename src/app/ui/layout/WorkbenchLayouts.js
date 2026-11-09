.pragma library

var panels = ["editor", "timeline", "preview", "sidebar", "validation", "muri"]

function group(tabs, selected) {
    return { tabs: tabs, selected: selected || tabs[0] }
}

function split(axis, ratio, first, second) {
    if (!first) return second
    if (!second) return first
    return { axis: axis, ratio: ratio, first: first, second: second }
}

function normalizedTree(tree) {
    const seen = []
    function visit(node, depth) {
        if (!node || depth >= panels.length)
            return null
        if (node.tabs && Number.isInteger(node.tabs.length)) {
            const storedTabs = []
            for (const id of Array.from(node.tabs)) {
                if (id === "diagnostics") storedTabs.push("validation", "muri")
                else storedTabs.push(id)
            }
            const tabs = storedTabs.filter(id => {
                if (!panels.includes(id) || seen.includes(id))
                    return false
                seen.push(id)
                return true
            })
            const selected = node.selected === "diagnostics" ? "validation" : node.selected
            return tabs.length ? group(tabs, tabs.includes(selected) ? selected : tabs[0]) : null
        }
        if (node.axis !== "horizontal" && node.axis !== "vertical")
            return null
        const ratio = Number.isFinite(node.ratio) ? Math.min(0.95, Math.max(0.05, node.ratio)) : 0.5
        return split(node.axis, ratio, visit(node.first, depth + 1), visit(node.second, depth + 1))
    }
    const normalized = visit(tree, 0)
    return seen.length === panels.length ? normalized : null
}

function normalizedFloating(floating) {
    const result = Object.assign({}, floating)
    if (result.diagnostics && !result.validation)
        result.validation = result.diagnostics
    delete result.diagnostics
    return result
}

function pathFor(node, id, path = "") {
    if (node.tabs)
        return node.tabs.includes(id) ? path : null
    return pathFor(node.first, id, path + "0") ?? pathFor(node.second, id, path + "1")
}

function replace(node, path, change) {
    if (path.length === 0)
        return change(node)
    const tail = path.slice(1)
    return path[0] === "0"
        ? split(node.axis, node.ratio, replace(node.first, tail, change), node.second)
        : split(node.axis, node.ratio, node.first, replace(node.second, tail, change))
}

function remove(node, id) {
    if (node.tabs) {
        const tabs = node.tabs.filter(tab => tab !== id)
        return tabs.length ? group(tabs, node.selected === id ? tabs[0] : node.selected) : null
    }
    return split(node.axis, node.ratio, remove(node.first, id), remove(node.second, id))
}

function dock(node, id, path, edge, beforeTab = "") {
    if (edge === "center") {
        // Insert before removing the old copy, so collapsing a split cannot invalidate the target path.
        const tagged = id + "@"
        const inserted = replace(node, path, target => {
            const tabs = target.tabs.slice()
            const index = tabs.indexOf(beforeTab)
            tabs.splice(index < 0 ? tabs.length : index, 0, tagged)
            return group(tabs, tagged)
        })
        return renameTab(remove(inserted, id), tagged, id)
    }
    const tagged = id + "@"
    const moving = group([tagged])
    const axis = edge === "left" || edge === "right" ? "horizontal" : "vertical"
    const before = edge === "left" || edge === "top"
    const inserted = replace(node, path, target => before
        ? split(axis, 0.32, moving, target) : split(axis, 0.68, target, moving))
    return renameTab(remove(inserted, id), tagged, id)
}

function renameTab(node, before, after) {
    if (node.tabs)
        return group(node.tabs.map(id => id === before ? after : id), node.selected === before ? after : node.selected)
    return split(node.axis, node.ratio, renameTab(node.first, before, after), renameTab(node.second, before, after))
}

function activate(node, id) {
    if (node.tabs)
        return node.tabs.includes(id) ? group(node.tabs, id) : node
    return split(node.axis, node.ratio, activate(node.first, id), activate(node.second, id))
}
