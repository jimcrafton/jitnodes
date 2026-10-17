#pragma once

#include <functional>

#include "newui/newui.h"
#include "newui/rootview.h"
#include "newui/runloop.h"
#include "newui/subview.h"
#include "newui/view.h"

#include "profileController.h"

namespace jitnodes::editor {

// Draws the profile rows (columns line up with the header labels of mainWindow.newui) and reports clicks on a row.
class ProfileCanvas : public newui::SubView {
public:
    explicit ProfileCanvas(ProfileController& controller);

    // Called with the node of a clicked root-graph row.
    std::function<void(NodeId)> onRowClicked;

    void paint(BLContext& ctx) override;

private:
    ProfileController& controller_;
};

}  // namespace jitnodes::editor
