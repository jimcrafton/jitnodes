#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "newui/newui.h"
#include "newui/controls.h"
#include "newui/subview.h"
#include "newui/view.h"

#include "editorModel.h"

namespace jitnodes::editor {

// Builds one row per parameter of the selected node inside a scroll view that fills `host` (a SubView from
// mainWindow.newui; rows are positioned by hand, so the list scrolls when it is taller than the panel) and writes edits back
// through the EditorModel: a text field (Enter or losing focus applies it), an fx button that switches a float between a
// value and an expression, a key button that sets a keyframe at the playhead, and a "..." button that opens the keyframe
// editor (onEditKeys gets the parameter name). Text parameters get a text field, and
// code gets a multi-line one. Rows are rebuilt only when the selected node or its parameter list changes.
class ParameterPanel {
public:
    // onEdited runs after the model changed (so the owner can recook and refresh); onMessage gets error text, or an
    // empty string when an edit succeeded.
    ParameterPanel(newui::SubView& host, EditorModel& model, std::function<void()> onEdited, std::function<void(const std::string&)> onMessage,
                   std::function<void(const std::string&)> onEditKeys);

    // Rebuilds the rows when needed and otherwise refreshes the field texts (except the one being edited).
    void sync();
    size_t rowCount() const { return rows_.size(); }
    // For tests and automation: the field of a row by parameter name, or nullptr.
    newui::View* fieldOf(const std::string& name) const;
    void applyField(const std::string& name);

private:
    struct Row {
        ParamInfo info;
        std::string shown;
        newui::SubView* row = nullptr;
        newui::TextField* field = nullptr;
        newui::TextControl* area = nullptr;
    };

    void rebuild(NodeId id, const std::vector<ParamInfo>& infos);
    void addRow(const ParamInfo& info);
    void commit(size_t index);
    void clear();
    void layoutRows();
    std::string fieldText(const Row& r) const;
    void setFieldText(Row& r, const std::string& text);

    newui::SubView& host_;
    newui::ScrollView* scroll_ = nullptr;
    newui::SubView* content_ = nullptr;
    EditorModel& model_;
    std::function<void()> onEdited_;
    std::function<void(const std::string&)> onMessage_;
    std::function<void(const std::string&)> onEditKeys_;
    NodeId node_ = invalidNodeId;
    std::vector<Row> rows_;
    bool rebuilding_ = false;
};

}  // namespace jitnodes::editor
