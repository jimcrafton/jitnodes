#include "keyframeDialog.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>

#include "drawText.h"
#include "newui/utils.h"
#ifdef JITNODES_GUI_AUTOMATION
#include "newui/automation.h"
#endif

namespace jitnodes::editor {

namespace {

constexpr float rowHeight = 26.0f;
constexpr float rowGap = 6.0f;

std::string number(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}

bool parseNumber(const std::string& text, double& out) {
    const char* begin = text.c_str();
    char* end = nullptr;
    out = std::strtod(begin, &end);
    if (end == begin) return false;
    while (*end == ' ') ++end;
    return *end == '\0';
}

template <class V>
V* make(newui::SubView& parent, float width, float height, float weight = 0.0f) {
    auto* v = new V();
    v->setBounds(newui::Rect(0, 0, width, height));
    v->setDesiredSize(newui::Size(width, height));
    if (weight > 0.0f) v->setLayoutParams(std::make_unique<newui::FlexLayoutParams>(weight));
    parent.addChild(v);
    return v;
}

newui::View* find(newui::RootView& root, const char* name) {
    newui::View* v = root.findView(name);
    if (!v) throw std::runtime_error(std::string("keyframeDialog.newui: no view named '") + name + "'");
    return v;
}

// The curve of the keys over time, the keys as squares and the playhead as a vertical line.
class CurveCanvas : public newui::SubView {
public:
    CurveCanvas(const KeyframeEditor& editor, double playhead) : editor_(editor), playhead_(playhead) { setVisible(true); }

    void paint(BLContext& ctx) override {
        const float w = bounds().width(), h = bounds().height();
        ctx.save();
        ctx.set_fill_style(BLRgba32(0xFF1A1B1E));
        ctx.fill_rect(BLRect(0, 0, w, h));
        const KeyframeEditor::Range r = editor_.range();
        const double span = r.t1 - r.t0;
        const double t0 = r.t0 - span * 0.1, t1 = r.t1 + span * 0.1;
        const float vspan = r.v1 - r.v0;
        const float v0 = r.v0 - vspan * 0.15f, v1 = r.v1 + vspan * 0.15f;
        const float margin = 8.0f;
        auto sx = [&](double t) { return margin + static_cast<float>((t - t0) / (t1 - t0)) * (w - 2 * margin); };
        auto sy = [&](float v) { return h - margin - (v - v0) / (v1 - v0) * (h - 2 * margin); };

        ctx.set_fill_style(BLRgba32(0xFF3A3D43));
        if (v0 < 0.0f && v1 > 0.0f) ctx.fill_rect(BLRect(0, sy(0.0f), w, 1));
        if (t0 < 0.0 && t1 > 0.0) ctx.fill_rect(BLRect(sx(0.0), 0, 1, h));
        ctx.set_fill_style(BLRgba32(0xFFE5A93D));
        ctx.fill_rect(BLRect(sx(playhead_), 0, 1, h));

        if (!editor_.keys().empty()) {
            BLPath path;
            const int steps = std::max(2, static_cast<int>(w / 3));
            for (int i = 0; i <= steps; ++i) {
                const double t = t0 + (t1 - t0) * i / steps;
                const float x = sx(t), y = sy(editor_.valueAt(t));
                if (i == 0) path.move_to(x, y);
                else path.line_to(x, y);
            }
            ctx.set_stroke_width(2.0f);
            ctx.set_stroke_style(BLRgba32(0xFF5A7CFF));
            ctx.stroke_path(path);
            ctx.set_fill_style(BLRgba32(0xFFE6E6E6));
            for (const Keyframe& k : editor_.keys()) ctx.fill_rect(BLRect(sx(k.time) - 3, sy(k.value) - 3, 6, 6));
        } else {
            drawText(ctx, "no keys", 11.0f, 10.0f, 20.0f, BLRgba32(0xFF9A9CA3));
        }
        ctx.restore();
    }

private:
    const KeyframeEditor& editor_;
    double playhead_;
};

}  // namespace

KeyframeDialog::KeyframeDialog(const std::string& title, std::vector<Keyframe> keys, double playhead)
    : editor_(std::move(keys)), playhead_(playhead) {
    dialog_.setName("keyframeDialog");
    if (!newui::Bundle::instance().loadFrame(dialog_)) throw std::runtime_error("could not load Resources/keyframeDialog.newui");
    dialog_.setTitle("Edit keyframes - " + title);
    newui::RootView& root = dialog_.rootView();
#ifdef JITNODES_GUI_AUTOMATION
    newui::Automation::expose(root);
#endif

    dynamic_cast<newui::Label*>(find(root, "dlgTitle"))->setText(title);
    dynamic_cast<newui::Label*>(find(root, "dlgInfo"))->setText("Playhead " + number(playhead) + " s");
    setMessage("");

    auto* previewHost = dynamic_cast<newui::SubView*>(find(root, "curvePreview"));
    rowHost_ = dynamic_cast<newui::SubView*>(find(root, "keyRows"));
    if (!previewHost || !rowHost_) throw std::runtime_error("keyframeDialog.newui: curvePreview and keyRows must be SubViews");

    previewHost->setLayout(std::make_unique<newui::FlexLayout>(newui::Orientation::Vertical));
    auto* curve = new CurveCanvas(editor_, playhead_);
    curve->setBounds(newui::Rect(0, 0, previewHost->bounds().width(), previewHost->bounds().height()));
    curve->setLayoutParams(std::make_unique<newui::FlexLayoutParams>(1.0f));
    previewHost->addChild(curve);
    preview_ = curve;

    rowHost_->setLayout(std::make_unique<newui::FlexLayout>(newui::Orientation::Vertical));
    scroll_ = new newui::ScrollView();
    scroll_->setBounds(newui::Rect(0, 0, rowHost_->bounds().width(), rowHost_->bounds().height()));
    scroll_->setLayoutParams(std::make_unique<newui::FlexLayoutParams>(1.0f));
    rowHost_->addChild(scroll_);
    content_ = new newui::SubView();
    content_->setVisible(true);
    content_->setBounds(newui::Rect(0, 0, rowHost_->bounds().width(), 10));
    scroll_->addChild(content_);

    auto click = [&](const char* name, std::function<void()> action) {
        auto* b = dynamic_cast<newui::Button*>(find(root, name));
        if (!b) throw std::runtime_error(std::string("keyframeDialog.newui: ") + name + " is not a button");
        b->onClick.add(std::function<newui::SyncReturn(newui::Control&)>([action](newui::Control&) {
            action();
            return newui::SyncReturn::Handled;
        }));
    };
    click("addKeyButton", [this] { schedule([this] { addKeyAtPlayhead(); }); });
    click("okButton", [this] { accept(); });
    click("cancelButton", [this] { dialog_.close(newui::DialogResult::Cancel); });

    rebuildNow();
}

KeyframeDialog::~KeyframeDialog() { *alive_ = false; }

bool KeyframeDialog::run(newui::Frame& owner, std::vector<Keyframe>& keys) {
    dialog_.showModal(&owner);
    if (dialog_.result() != newui::DialogResult::Ok) return false;
    keys = editor_.keys();
    return true;
}

void KeyframeDialog::schedule(std::function<void()> task) {
    std::shared_ptr<bool> alive = alive_;
    // postIdle, not post: the modal loop runs idle tasks but drops the thread message post() relies on.
    newui::Application::instance().runLoop().postIdle([alive, task = std::move(task)] {
        if (*alive) task();
        return true;
    });
}

void KeyframeDialog::setMessage(const std::string& text) {
    if (auto* label = dynamic_cast<newui::Label*>(dialog_.rootView().findView("dlgMessage"))) label->setText(text.empty() ? std::string("·") : text);
}

void KeyframeDialog::changed() {
    if (preview_) preview_->redraw();
}

void KeyframeDialog::rebuildNow() {
    rebuilding_ = true;
    rows_.clear();
    while (!content_->childViews().empty()) content_->childViews().front()->destroy();
    rebuilding_ = false;
    for (size_t i = 0; i < editor_.size(); ++i) addRow(i);
    layoutRows();
    changed();
}

void KeyframeDialog::layoutRows() {
    const float width = std::max(200.0f, scroll_->bounds().width() - 18.0f);
    float y = 0.0f;
    for (Row& r : rows_) {
        r.row->setBounds(newui::Rect(0, y, width, rowHeight));
        y += rowHeight + rowGap;
    }
    const float total = std::max(1.0f, y - rowGap);
    content_->setBounds(newui::Rect(0, 0, width, total));
    scroll_->setContentSize(newui::Size(width, total));
}

void KeyframeDialog::addRow(size_t index) {
    const Keyframe& key = editor_.keys()[index];
    auto* row = new newui::SubView();
    row->setVisible(true);
    row->setBounds(newui::Rect(0, 0, 300, rowHeight));
    row->setDesiredSize(newui::Size(0, rowHeight));
    auto layout = std::make_unique<newui::FlexLayout>(newui::Orientation::Horizontal);
    layout->setSpacing(6.0f);
    layout->setCrossAxisAlignment(newui::CrossAxisAlignment::Center);
    row->setLayout(std::move(layout));
    content_->addChild(row);

    Row r;
    r.row = row;
    r.time = make<newui::TextField>(*row, 96, 24);
    r.value = make<newui::TextField>(*row, 96, 24);
    r.interp = make<newui::DropDownList>(*row, 120, 24);
    auto items = std::make_unique<newui::StringListModel>();
    for (KeyInterp k : {KeyInterp::constant, KeyInterp::linear, KeyInterp::smooth}) items->addItem(interpName(k));
    r.interp->setModel(std::move(items));
    r.interp->setSelectedIndex(static_cast<size_t>(key.interp));
    auto* remove = make<newui::Button>(*row, 30, 24);
    remove->setText("x");
    const std::string suffix = "." + std::to_string(index);
    r.time->setName("keyTime" + suffix);
    r.value->setName("keyValue" + suffix);
    r.interp->setName("keyInterp" + suffix);
    remove->setName("keyDelete" + suffix);

    r.shownTime = number(key.time);
    r.shownValue = number(key.value);
    r.time->setText(newui::utf8ToWide(r.shownTime));
    r.value->setText(newui::utf8ToWide(r.shownValue));

    for (newui::TextField* field : {r.time, r.value}) {
        field->onReturnPressed.add(std::function<newui::SyncReturn(newui::TextField&)>([this, index](newui::TextField&) {
            if (!rebuilding_) applyRow(index);
            return newui::SyncReturn::Handled;
        }));
        field->onLostFocus.add(std::function<newui::SyncReturn(newui::View&)>([this, index](newui::View&) {
            if (!rebuilding_) applyRow(index);
            return newui::SyncReturn::Handled;
        }));
    }
    r.interp->onSelectionChanged.add(std::function<newui::SyncReturn(newui::DropDownList&)>([this, index](newui::DropDownList& d) {
        if (!rebuilding_ && d.selectedIndex()) setInterpolation(index, static_cast<KeyInterp>(*d.selectedIndex()));
        return newui::SyncReturn::Handled;
    }));
    remove->onClick.add(std::function<newui::SyncReturn(newui::Control&)>([this, index](newui::Control&) {
        schedule([this, index] { removeKey(index); });
        return newui::SyncReturn::Handled;
    }));
    rows_.push_back(r);
}

std::string KeyframeDialog::applyRow(size_t row) {
    if (row >= rows_.size()) return "no such row";
    Row& r = rows_[row];
    const std::string timeText = newui::wideToUtf8(r.time->text()), valueText = newui::wideToUtf8(r.value->text());
    std::string error;
    size_t index = row;
    bool reorder = false;

    if (timeText != r.shownTime) {
        double t = 0.0;
        if (!parseNumber(timeText, t)) error = "the time must be a number";
        else if ((error = editor_.setTime(index, t)).empty()) reorder = index != row;
    }
    if (error.empty() && valueText != r.shownValue) {
        double v = 0.0;
        if (!parseNumber(valueText, v)) error = "the value must be a number";
        else error = editor_.setValue(index, static_cast<float>(v));
    }
    setMessage(error);
    if (!error.empty()) {
        r.time->setText(newui::utf8ToWide(r.shownTime));
        r.value->setText(newui::utf8ToWide(r.shownValue));
        return error;
    }
    r.shownTime = timeText;
    r.shownValue = valueText;
    reordered_ = reorder;
    changed();
    if (reorder) schedule([this] { rebuildNow(); });
    return {};
}

std::string KeyframeDialog::setInterpolation(size_t row, KeyInterp interp) {
    const std::string error = editor_.setInterp(row, interp);
    setMessage(error);
    changed();
    return error;
}

void KeyframeDialog::removeKey(size_t row) {
    const std::string error = editor_.remove(row);
    setMessage(error);
    rebuildNow();
}

void KeyframeDialog::addKeyAtPlayhead() {
    size_t index = 0;
    setMessage(editor_.addAt(playhead_, index));
    rebuildNow();
}

bool KeyframeDialog::accept() {
    for (size_t i = 0; i < rows_.size();) {
        reordered_ = false;
        if (!applyRow(i).empty()) return false;
        if (reordered_) {
            rebuildNow();
            i = 0;
            continue;
        }
        ++i;
    }
    dialog_.close(newui::DialogResult::Ok);
    return true;
}

}  // namespace jitnodes::editor
