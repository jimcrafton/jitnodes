#pragma once

#include <memory>
#include <string>
#include <vector>

#include "newui/newui.h"
#include "newui/application.h"
#include "newui/bundle.h"
#include "newui/controls.h"
#include "newui/dialogs.h"
#include "newui/frame.h"
#include "newui/rootview.h"
#include "newui/runloop.h"
#include "newui/subview.h"
#include "newui/view.h"

#include "keyframeEditor.h"

namespace jitnodes::editor {

// Modal dialog (keyframeDialog.newui) that edits the keyframes of one parameter: a curve preview, and one row per key with
// time, value, interpolation and a delete button. Edits go to a KeyframeEditor working copy; OK hands the keys back,
// Cancel discards them. The row operations are public so they can be driven without a mouse.
class KeyframeDialog {
public:
    // Throws std::runtime_error when Resources/keyframeDialog.newui cannot be loaded or lacks a view.
    KeyframeDialog(const std::string& title, std::vector<Keyframe> keys, double playhead);
    ~KeyframeDialog();
    KeyframeDialog(const KeyframeDialog&) = delete;
    KeyframeDialog& operator=(const KeyframeDialog&) = delete;

    // Shows the dialog over `owner` and blocks until it closes; true (and the keys) when it was closed with OK.
    bool run(newui::Frame& owner, std::vector<Keyframe>& keys);

    const KeyframeEditor& editor() const { return editor_; }
    size_t rowCount() const { return rows_.size(); }
    newui::TextField& timeField(size_t row) { return *rows_.at(row).time; }
    newui::TextField& valueField(size_t row) { return *rows_.at(row).value; }
    newui::View& preview() { return *preview_; }

    // Applies the texts typed into a row; the rows are rebuilt afterwards when the key order changed. Returns an error text.
    std::string applyRow(size_t row);
    std::string setInterpolation(size_t row, KeyInterp interp);
    void removeKey(size_t row);
    void addKeyAtPlayhead();
    void rebuildNow();
    bool accept();  // applies pending edits and closes with OK; false (stays open) when an edit is invalid

private:
    struct Row {
        newui::SubView* row = nullptr;
        newui::TextField* time = nullptr;
        newui::TextField* value = nullptr;
        newui::DropDownList* interp = nullptr;
        std::string shownTime;
        std::string shownValue;
    };

    void addRow(size_t index);
    void layoutRows();
    void schedule(std::function<void()> task);
    void setMessage(const std::string& text);
    void changed();

    newui::Dialog dialog_;
    KeyframeEditor editor_;
    double playhead_;
    newui::SubView* rowHost_ = nullptr;
    newui::ScrollView* scroll_ = nullptr;
    newui::SubView* content_ = nullptr;
    newui::SubView* preview_ = nullptr;
    std::vector<Row> rows_;
    bool rebuilding_ = false;
    bool reordered_ = false;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace jitnodes::editor
