#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace stylus {

// Undo/redo built from small closures rather than diffing the whole document.
// Strokes store only the pixels they touched, so a long stroke stays cheap.
class History {
public:
    struct Entry {
        std::string label;
        std::function<void()> undo;
        std::function<void()> redo;
    };

    explicit History(size_t limit = 80) : limit_(limit) {}

    void push(std::string label, std::function<void()> undo, std::function<void()> redo);

    bool canUndo() const { return index_ >= 0; }
    bool canRedo() const {
        return index_ + 1 >= 0 && static_cast<size_t>(index_ + 1) < entries_.size();
    }

    // Returns the label of the action performed, or std::nullopt when there
    // was nothing to undo/redo.
    std::optional<std::string> undo();
    std::optional<std::string> redo();

    void clear();

    size_t size() const { return entries_.size(); }
    long indexAsLong() const { return index_; }

private:
    std::vector<Entry> entries_;
    long index_ = -1;
    size_t limit_;
};

}  // namespace stylus