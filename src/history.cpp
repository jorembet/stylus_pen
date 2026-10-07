#include "history.h"

#include <algorithm>

namespace stylus {

void History::push(std::string label, std::function<void()> undo, std::function<void()> redo) {
    // Anything after the cursor was undone, so a new action discards it.
    if (index_ + 1 < static_cast<long>(entries_.size())) {
        entries_.erase(entries_.begin() + index_ + 1, entries_.end());
    }
    entries_.push_back(Entry{std::move(label), std::move(undo), std::move(redo)});
    if (entries_.size() > limit_) entries_.erase(entries_.begin());
    index_ = static_cast<long>(entries_.size()) - 1;
}

std::optional<std::string> History::undo() {
    if (!canUndo()) return std::nullopt;
    Entry& entry = entries_[static_cast<size_t>(index_)];
    if (entry.undo) entry.undo();
    std::string label = entry.label;
    index_ -= 1;
    return label;
}

std::optional<std::string> History::redo() {
    if (!canRedo()) return std::nullopt;
    Entry& entry = entries_[static_cast<size_t>(index_ + 1)];
    if (entry.redo) entry.redo();
    std::string label = entry.label;
    index_ += 1;
    return label;
}

void History::clear() {
    entries_.clear();
    index_ = -1;
}

}  // namespace stylus