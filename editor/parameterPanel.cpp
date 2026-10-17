#include "parameterPanel.h"

#include <algorithm>

#include "newui/utils.h"

namespace jitnodes::editor {

namespace {

constexpr float rowHeight = 26.0f;
constexpr float areaHeight = 96.0f;
constexpr float rowGap = 6.0f;

newui::Color plainColor() { return newui::Color(0xFF202020u); }
newui::Color expressionColor() { return newui::Color(0xFFB45F00u); }
newui::Color keyedColor() { return newui::Color(0xFF1F7A3Au); }

newui::Color colorFor(const ParamInfo& p) { return p.expression ? expressionColor() : (p.keyed ? keyedColor() : plainColor()); }

template <class V>
V* make(newui::SubView& parent, float width, float height, float weight = 0.0f) {
    auto* v = new V();
    v->setBounds(newui::Rect(0, 0, width, height));
    v->setDesiredSize(newui::Size(width, height));
    if (weight > 0.0f) v->setLayoutParams(std::make_unique<newui::FlexLayoutParams>(weight));
    parent.addChild(v);
    return v;
}

}  // namespace

ParameterPanel::ParameterPanel(newui::SubView& host, EditorModel& model, std::function<void()> onEdited, std::function<void(const std::string&)> onMessage,
                               std::function<void(const std::string&)> onEditKeys)
    : host_(host), model_(model), onEdited_(std::move(onEdited)), onMessage_(std::move(onMessage)), onEditKeys_(std::move(onEditKeys)) {
    auto layout = std::make_unique<newui::FlexLayout>(newui::Orientation::Vertical);
    host_.setLayout(std::move(layout));

    scroll_ = new newui::ScrollView();
    scroll_->setBounds(newui::Rect(0, 0, host_.bounds().width(), host_.bounds().height()));
    scroll_->setLayoutParams(std::make_unique<newui::FlexLayoutParams>(1.0f));
    host_.addChild(scroll_);
    content_ = new newui::SubView();
    content_->setVisible(true);
    content_->setBounds(newui::Rect(0, 0, host_.bounds().width(), 10));
    scroll_->addChild(content_);
}

void ParameterPanel::layoutRows() {
    const float width = std::max(120.0f, scroll_->bounds().width() - 18.0f);
    float y = 0.0f;
    for (Row& r : rows_) {
        const float height = r.info.multiline ? areaHeight : rowHeight;
        r.row->setBounds(newui::Rect(0, y, width, height));
        y += height + rowGap;
    }
    const float total = std::max(1.0f, y - rowGap);
    content_->setBounds(newui::Rect(0, 0, width, total));
    scroll_->setContentSize(newui::Size(width, total));
}

void ParameterPanel::clear() {
    rebuilding_ = true;
    rows_.clear();
    while (!content_->childViews().empty()) content_->childViews().front()->destroy();
    rebuilding_ = false;
}

std::string ParameterPanel::fieldText(const Row& r) const {
    return r.area ? newui::wideToUtf8(r.area->text()) : newui::wideToUtf8(r.field->text());
}

void ParameterPanel::setFieldText(Row& r, const std::string& text) {
    if (r.area) r.area->setText(newui::utf8ToWide(text));
    else r.field->setText(newui::utf8ToWide(text));
    r.shown = text;
}

void ParameterPanel::sync() {
    const NodeId id = model_.selected();
    std::vector<ParamInfo> infos;
    if (id != invalidNodeId) infos = model_.parameters(id);

    bool same = id == node_ && infos.size() == rows_.size();
    for (size_t i = 0; same && i < infos.size(); ++i) same = infos[i].name == rows_[i].info.name && infos[i].isText == rows_[i].info.isText;
    if (!same) {
        rebuild(id, infos);
        return;
    }
    for (size_t i = 0; i < rows_.size(); ++i) {
        Row& r = rows_[i];
        r.info = infos[i];
        newui::View* editor = r.area ? static_cast<newui::View*>(r.area) : static_cast<newui::View*>(r.field);
        if (!editor->isFocused() && r.shown != infos[i].text) setFieldText(r, infos[i].text);
        if (r.field) r.field->setTextColor(colorFor(infos[i]));
    }
    layoutRows();
}

void ParameterPanel::rebuild(NodeId id, const std::vector<ParamInfo>& infos) {
    clear();
    node_ = id;
    for (const ParamInfo& info : infos) addRow(info);
    layoutRows();
}

void ParameterPanel::addRow(const ParamInfo& info) {
    const size_t index = rows_.size();
    const float height = info.multiline ? areaHeight : rowHeight;
    auto* row = new newui::SubView();
    row->setVisible(true);
    row->setBounds(newui::Rect(0, 0, 300, height));
    row->setDesiredSize(newui::Size(0, height));
    auto layout = std::make_unique<newui::FlexLayout>(newui::Orientation::Horizontal);
    layout->setSpacing(6.0f);
    layout->setCrossAxisAlignment(info.multiline ? newui::CrossAxisAlignment::Start : newui::CrossAxisAlignment::Center);
    row->setLayout(std::move(layout));
    content_->addChild(row);

    auto* name = make<newui::Label>(*row, 84, 22);
    name->setText(info.name);
    name->setTextColor(BLRgba32(0xFF9A9CA3));

    Row r;
    r.info = info;
    r.row = row;
    if (info.multiline) {
        r.area = make<newui::TextControl>(*row, 100, height, 1.0f);
        r.area->setWordWrap(true);
        r.area->onLostFocus.add(std::function<newui::SyncReturn(newui::View&)>([this, index](newui::View&) {
            if (!rebuilding_) commit(index);
            return newui::SyncReturn::Handled;
        }));
    } else {
        r.field = make<newui::TextField>(*row, 100, 24, 1.0f);
        r.field->setTextColor(colorFor(info));
        r.field->onReturnPressed.add(std::function<newui::SyncReturn(newui::TextField&)>([this, index](newui::TextField&) {
            if (!rebuilding_) commit(index);
            return newui::SyncReturn::Handled;
        }));
        r.field->onLostFocus.add(std::function<newui::SyncReturn(newui::View&)>([this, index](newui::View&) {
            if (!rebuilding_) commit(index);
            return newui::SyncReturn::Handled;
        }));
    }

    if (!info.isText) {
        const std::string paramName = info.name;
        auto* fx = make<newui::Button>(*row, 30, 24);
        fx->setText("fx");
        fx->onClick.add(std::function<newui::SyncReturn(newui::Control&)>([this, paramName](newui::Control&) {
            onMessage_(model_.toggleExpression(node_, paramName));
            onEdited_();
            return newui::SyncReturn::Handled;
        }));
        auto* key = make<newui::Button>(*row, 30, 24);
        key->setText("key");
        key->onClick.add(std::function<newui::SyncReturn(newui::Control&)>([this, paramName](newui::Control&) {
            onMessage_(model_.keyframeNow(node_, paramName));
            onEdited_();
            return newui::SyncReturn::Handled;
        }));
        auto* keys = make<newui::Button>(*row, 30, 24);
        keys->setText("...");
        keys->onClick.add(std::function<newui::SyncReturn(newui::Control&)>([this, paramName](newui::Control&) {
            if (onEditKeys_) onEditKeys_(paramName);
            return newui::SyncReturn::Handled;
        }));
    }

    rows_.push_back(r);
    setFieldText(rows_.back(), info.text);
}

void ParameterPanel::commit(size_t index) {
    if (index >= rows_.size()) return;
    Row& r = rows_[index];
    const std::string text = fieldText(r);
    if (text == r.shown) return;
    const std::string error = model_.setParameter(node_, r.info.name, text);
    onMessage_(error);
    if (!error.empty()) setFieldText(r, r.shown);
    else r.shown = text;
    onEdited_();
}

newui::View* ParameterPanel::fieldOf(const std::string& name) const {
    for (const Row& r : rows_)
        if (r.info.name == name) return r.area ? static_cast<newui::View*>(r.area) : static_cast<newui::View*>(r.field);
    return nullptr;
}

void ParameterPanel::applyField(const std::string& name) {
    for (size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].info.name == name) commit(i);
}

}  // namespace jitnodes::editor
