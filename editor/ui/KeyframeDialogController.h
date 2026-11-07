#pragma once

#include <newui/rootcontroller.h>
#include <newui/rootviewproxy.h>
#include <newui/view.h>
#include <newui/geometry.h>

class KeyframeDialogController : public newui::RootController {
public:
    using RootController::RootController;

private:
    //@reflect connect=true
    newui::RootViewProxy* rootview_ = nullptr;

private:
    newui::SyncReturn onRootviewSizeChanged(newui::View& sender, const newui::Size& size) {
        // TODO: handle onSizeChanged
        return newui::SyncReturn::Handled;
    }

protected:
    bool internal_init() override {
        if (!Component::internal_init()) {
            return false;
        }
        return true;
    }
};
