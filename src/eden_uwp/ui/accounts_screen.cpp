// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/ui/accounts_screen.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>

#include "common/fs/fs.h"
#include "common/scope_exit.h"
#include "common/stb.h"
#include "core/constants_nxbox.h"
#include "core/hle/service/acc/profile_manager.h"
#include "eden_uwp/accounts.h"
#include "eden_uwp/accounts_policy.h"
#include "eden_uwp/diagnostic.h"
#include "eden_uwp/moru_parts.h"
#include "eden_uwp/ui/avatar_composite.h"
#include "eden_uwp/ui/moru_screen.h"
#include "eden_uwp/ui/strings.h"
#include "eden_uwp/ui/text_entry.h"
#include "eden_uwp/ui/widgets.h"

namespace EdenXbox::Ui {
namespace {
using D2D1::RectF;
using winrt::Windows::UI::Core::CoreWindow;
using Manager = Service::Account::ProfileManager;

enum class Page { List, Detail, Picture, Presets, Colors, Remove, DeleteSaves };
constexpr std::array<const char*, 8> kCategories = {"animals", "fantasy",  "sci-fi", "sports",
                                                    "food",    "monsters", "robots", "people"};
constexpr std::array<Text, 8> kCategoryLabels = {
    Text::AvatarAnimals, Text::AvatarFantasy,  Text::AvatarSciFi,  Text::AvatarSports,
    Text::AvatarFood,    Text::AvatarMonsters, Text::AvatarRobots, Text::AvatarPeople};

constexpr std::array<Text, 40> kAvatarColorLabels = {
    Text::AvatarRed,      Text::AvatarOrange, Text::AvatarYellow,     Text::AvatarLime,
    Text::AvatarGreen,    Text::AvatarTeal,   Text::AvatarSkyBlue,    Text::AvatarBlue,
    Text::AvatarPurple,   Text::AvatarPink,   Text::AvatarWhite,      Text::AvatarBlack,
    Text::AvatarRose,     Text::AvatarPeach,  Text::AvatarButter,     Text::AvatarMint,
    Text::AvatarSeafoam,  Text::AvatarIce,    Text::AvatarPeriwinkle, Text::AvatarLavender,
    Text::AvatarLilac,    Text::AvatarCream,  Text::AvatarNavy,       Text::AvatarForest,
    Text::AvatarPetrol,   Text::AvatarPlum,   Text::AvatarBurgundy,   Text::AvatarChocolate,
    Text::AvatarCharcoal, Text::AvatarSlate,  Text::AvatarCopper,     Text::AvatarSand,
    Text::AvatarOlive,    Text::AvatarCoral,  Text::AvatarOcean,      Text::AvatarRaspberry,
    Text::AvatarSunrise,  Text::AvatarLagoon, Text::AvatarDusk,       Text::AvatarEmber};

class AccountsScreen {
public:
    AccountsScreen(Renderer& renderer, const CoreWindow& window, Input& input,
                   const std::filesystem::path& local_state, bool launch, bool picture_only = false)
        : renderer_(renderer), window_(window), input_(input), local_state_(local_state),
          state_(Accounts::Load(local_state)), picture_only_(picture_only), launch_(launch) {
        page_ = picture_only_ ? Page::Picture : Page::List;
        if (picture_only_ && state_.multi && !state_.selected_uuid.empty())
            account_ = profiles_.GetUserIndex(Common::UUID{state_.selected_uuid}).value_or(0);
        FilterCharacters();
        if (launch_ && state_.multi) {
            row_ = static_cast<int>(
                profiles_
                    .GetUserIndex(Common::UUID{state_.selected_uuid.empty()
                                                   ? profiles_.GetUser(0)->RawString()
                                                   : state_.selected_uuid})
                    .value_or(0));
        }
        ReloadImages();
    }

    bool Run() {
        const InputTransition transition(input_);
        const auto token = window_.Closed([this](const auto&, const auto&) { closed_ = true; });
        SCOPE_EXIT {
            window_.Closed(token);
        };
        Diagnostic("ACCOUNTS open");
        while (!closed_ && !done_) {
            window_.Dispatcher().ProcessEvents(
                winrt::Windows::UI::Core::CoreProcessEventsOption::ProcessAllIfPresent);
            input_.Update();
            Update();
            renderer_.BeginFrame();
            Draw();
            renderer_.EndFrame();
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
        Diagnostic("ACCOUNTS close");
        return !closed_ && accepted_;
    }
    bool Closed() const {
        return closed_;
    }

private:
    bool Persist() {
        if (!Accounts::Save(local_state_, state_)) {
            failed_ = true;
            return false;
        }
        failed_ = false;
        Accounts::Apply(local_state_);
        return true;
    }

    void ReloadImages() {
        for (std::size_t i = 0; i < profiles_.GetUserCount(); ++i) {
            images_[i] = {};
            Pixels pixels;
            if (renderer_.DecodeImage(ReadFileBytes(Manager::AvatarPath(*profiles_.GetUser(i))),
                                      pixels))
                renderer_.UploadImage(pixels, images_[i]);
        }
    }

    void LoadPresets() {
        for (const auto index : visible_) {
            if (presets_[index].brush)
                continue;
            Pixels pixels;
            if (!renderer_.DecodeImage(CharacterBytes(index), pixels) ||
                !renderer_.UploadImage(pixels, presets_[index]))
                failed_ = true;
        }
    }

    void ShowPresets() {
        page_ = Page::Presets;
        row_ = 0;
        LoadPresets();
    }

    std::vector<u8> CharacterBytes(std::size_t index) const {
        const std::filesystem::path installed{std::wstring_view(
            winrt::Windows::ApplicationModel::Package::Current().InstalledLocation().Path())};
        return ReadFileBytes(installed / "Assets" / "avatars" /
                             (std::string(Core::Constants::NXBOX_CHARACTERS[index].id) + ".png"));
    }

    void FilterCharacters() {
        visible_.clear();
        for (std::size_t i = 0; i < Core::Constants::NXBOX_CHARACTERS.size(); ++i)
            if (std::string_view(Core::Constants::NXBOX_CHARACTERS[i].category) ==
                kCategories[category_])
                visible_.push_back(i);
        row_ = 0;
        if (page_ == Page::Presets)
            LoadPresets();
    }

    std::wstring Name(std::size_t index) const {
        Service::Account::ProfileBase profile{};
        profiles_.GetProfileBase(index, profile);
        const auto end = std::find(profile.username.begin(), profile.username.end(), 0);
        return Widen(std::string(profile.username.begin(), end));
    }

    void Select() {
        state_.selected_uuid = profiles_.GetUser(account_)->RawString();
        if (Persist()) {
            if (launch_) {
                accepted_ = true;
                done_ = true;
            } else {
                page_ = Page::List;
                row_ = static_cast<int>(account_) + 1;
            }
        }
    }

    void Update() {
        if (entry_) {
            const auto result = entry_->Update(input_);
            if (result == TextEntry::Result::Editing)
                return;
            if (result == TextEntry::Result::Confirmed) {
                const auto name = winrt::to_string(winrt::hstring(entry_->Text()));
                // A Switch username is at most 32 UTF-8 bytes, not 32 codepoints.
                if (name.empty() || name.size() > Service::Account::profile_username_size) {
                    failed_ = true;
                } else if (adding_) {
                    const auto uuid = Common::UUID::MakeRandom();
                    failed_ = profiles_.CreateNewUser(uuid, name).IsError();
                    if (!failed_) {
                        account_ = profiles_.GetUserCount() - 1;
                        failed_ = !profiles_.SetAvatar(uuid, Core::Constants::NXBOX_ACCOUNT_JPEG);
                        profiles_.WriteUserSaveFile();
                        ShowPresets();
                        ReloadImages();
                    }
                } else {
                    Service::Account::ProfileBase profile{};
                    const auto uuid = *profiles_.GetUser(account_);
                    profiles_.GetProfileBase(uuid, profile);
                    profile.username = {};
                    std::copy(name.begin(), name.end(), profile.username.begin());
                    failed_ = !profiles_.SetProfileBase(uuid, profile);
                }
            }
            entry_.reset();
            input_.ConsumeUntilRelease();
            return;
        }
        if (input_.Pressed(Button::B)) {
            if (page_ == Page::Colors) {
                page_ = moru_ ? (picture_only_ ? Page::Picture : Page::Detail) : Page::Presets;
                row_ =
                    moru_
                        ? (picture_only_ ? 1 : 5)
                        : static_cast<int>(std::find(visible_.begin(), visible_.end(), static_cast<std::size_t>(character_)) -
                                           visible_.begin());
            } else if (page_ == Page::Presets) {
                page_ = picture_only_ ? Page::Picture : Page::Detail;
                row_ = picture_only_ ? 0 : 2;
            } else if (page_ == Page::List) {
                done_ = true;
            } else if (picture_only_) {
                done_ = true;
            } else {
                page_ = Page::List;
                row_ = 0;
            }
            return;
        }
        if (page_ == Page::Presets) {
            if (input_.Pressed(Button::LeftShoulder) || input_.Pressed(Button::RightShoulder)) {
                category_ = (category_ + (input_.Pressed(Button::RightShoulder) ? 1 : 7)) % 8;
                FilterCharacters();
            }
            if (input_.Pressed(Button::Left))
                row_ = std::max(0, row_ - 1);
            if (input_.Pressed(Button::Right))
                row_ = std::min(static_cast<int>(visible_.size()) - 1, row_ + 1);
            if (input_.Pressed(Button::Up))
                row_ = std::max(0, row_ - 6);
            if (input_.Pressed(Button::Down))
                row_ = std::min(static_cast<int>(visible_.size()) - 1, row_ + 6);
        } else if (page_ == Page::Colors) {
            const int previous = row_;
            if (input_.Pressed(Button::Left))
                row_ = std::max(0, row_ - 1);
            if (input_.Pressed(Button::Right))
                row_ = std::min(static_cast<int>(Core::Constants::NXBOX_AVATAR_COLORS.size()) - 1,
                                row_ + 1);
            if (input_.Pressed(Button::Up))
                row_ = std::max(0, row_ - 10);
            if (input_.Pressed(Button::Down))
                row_ = std::min(static_cast<int>(Core::Constants::NXBOX_AVATAR_COLORS.size()) - 1,
                                row_ + 10);
            if (previous != row_)
                UpdatePreview();
        } else {
            int count = 2;
            if (page_ == Page::List)
                count =
                    launch_
                        ? static_cast<int>(profiles_.GetUserCount())
                        : (state_.multi
                               ? 1 + static_cast<int>(profiles_.GetUserCount()) +
                                     (profiles_.GetUserCount() < Service::Account::MAX_USERS ? 1
                                                                                             : 0)
                               : 1);
            if (page_ == Page::Detail)
                count = 6;
            if (input_.Pressed(Button::Up) || input_.Pressed(Button::Left))
                row_ = std::max(0, row_ - 1);
            if (input_.Pressed(Button::Down) || input_.Pressed(Button::Right))
                row_ = std::min(count - 1, row_ + 1);
        }
        if (!input_.Pressed(Button::A))
            return;
        switch (page_) {
        case Page::List:
            if (!launch_ && row_ == 0) {
                const auto previous = state_;
                state_.initialized = true;
                state_.multi = !state_.multi;
                if (!Persist())
                    state_ = previous;
                row_ = 0;
            } else if (!launch_ && row_ > static_cast<int>(profiles_.GetUserCount())) {
                adding_ = true;
                entry_ = std::make_unique<TextEntry>(Tr(Text::AccountName), L"");
            } else {
                account_ = static_cast<std::size_t>(row_ - (launch_ ? 0 : 1));
                if (launch_)
                    Select();
                else {
                    page_ = Page::Detail;
                    row_ = 0;
                }
            }
            break;
        case Page::Picture:
            if (row_ == 0) {
                ShowPresets();
            } else {
                ChooseMoru();
            }
            break;
        case Page::Detail:
            if (row_ == 0)
                Select();
            else if (row_ == 1) {
                adding_ = false;
                entry_ = std::make_unique<TextEntry>(Tr(Text::AccountName), Name(account_));
            } else if (row_ == 2) {
                ShowPresets();
            } else if (row_ == 4) {
                if (RunMoruScreen(renderer_, window_, input_))
                    closed_ = true;
            } else if (row_ == 5) {
                ChooseMoru();
            } else if (row_ == 3 && profiles_.GetUserCount() > 1) {
                page_ = Page::Remove;
                row_ = 0;
            }
            break;
        case Page::Presets:
            moru_.reset();
            character_ = static_cast<int>(visible_[row_]);
            page_ = Page::Colors;
            row_ = 0;
            UpdatePreview();
            break;
        case Page::Colors:
            failed_ = !SaveAvatar();
            if (!failed_) {
                ReloadImages();
                page_ = picture_only_ ? Page::Picture : Page::Detail;
                row_ = picture_only_ ? 0 : 2;
            }
            break;
        case Page::Remove:
            page_ = row_ == 1 ? Page::DeleteSaves : Page::Detail;
            row_ = 0;
            break;
        case Page::DeleteSaves: {
            const auto uuid = *profiles_.GetUser(account_);
            if (row_ == 1 && !Accounts::DeleteSaves(uuid.RawString())) {
                failed_ = true;
                break;
            }
            failed_ = !profiles_.RemoveUser(uuid);
            if (!failed_) {
                (void)Common::FS::RemoveFile(Manager::AvatarPath(uuid));
                if (state_.selected_uuid == uuid.RawString())
                    state_.selected_uuid = profiles_.GetUser(0)->RawString();
                Persist();
                ReloadImages();
                page_ = Page::List;
                row_ = 0;
            }
            break;
        }
        }
        input_.ConsumeUntilRelease();
    }

    void ChooseMoru() {
        if (RunMoruScreen(renderer_, window_, input_, &moru_))
            closed_ = true;
        if (moru_) {
            page_ = Page::Colors;
            row_ = 0;
            UpdatePreview();
        }
    }

    bool UpdatePreview() {
        preview_ = {};
        composite_ = {};
        if (moru_) {
            composite_.width = composite_.height = 256;
            composite_.bgra = Moru::Rasterize(Moru::MakeScene(*moru_));
        } else {
            if (!renderer_.DecodeImage(CharacterBytes(character_), composite_) ||
                composite_.width != 256 || composite_.height != 256) {
                failed_ = true;
                return false;
            }
        }
        const auto& color = Core::Constants::NXBOX_AVATAR_COLORS[row_];
        for (std::size_t i = 0; i < composite_.bgra.size(); i += 4) {
            const auto alpha = composite_.bgra[i + 3];
            const auto y = static_cast<unsigned>((i / 4) / 256);
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const auto background =
                    AvatarBackgroundChannel(color.rgb[2 - channel], color.end[2 - channel], y);
                composite_.bgra[i + channel] =
                    CompositeAvatarChannel(composite_.bgra[i + channel], alpha, background);
            }
            composite_.bgra[i + 3] = 255;
        }
        failed_ = !renderer_.UploadImage(composite_, preview_);
        return !failed_;
    }

    bool SaveAvatar() {
        if (!UpdatePreview())
            return false;
        std::vector<u8> rgb(256 * 256 * 3);
        for (std::size_t pixel = 0; pixel < 256 * 256; ++pixel)
            for (std::size_t channel = 0; channel < 3; ++channel)
                rgb[pixel * 3 + channel] = composite_.bgra[pixel * 4 + 2 - channel];
        // Use the guest's existing stb encoder; never truncate an oversized JPEG.
        for (int quality : {90, 75, 50, 25}) {
            std::vector<u8> jpeg;
            const auto append = [](void* context, void* data, int length) {
                auto& output = *static_cast<std::vector<u8>*>(context);
                const auto* bytes = static_cast<const u8*>(data);
                output.insert(output.end(), bytes, bytes + length);
            };
            if (!stbi_write_jpg_to_func(append, &jpeg, 256, 256, 3, rgb.data(), quality))
                return false;
            if (jpeg.size() <= 0x20000)
                return profiles_.SetAvatar(*profiles_.GetUser(account_), jpeg);
        }
        return false;
    }

    void DrawRow(int row, const std::wstring& label, const Image* image = nullptr) {
        const float y = 285.0f + static_cast<float>(row) * 62.0f;
        const auto rect = RectF(96.0f, y, 1500.0f, y + 54.0f);
        if (image && image->brush)
            renderer_.DrawImage(*image, RectF(108.0f, y, 162.0f, y + 54.0f), 27.0f, 1.0f);
        renderer_.DrawString(label, Font::Nav,
                             RectF(image ? 190.0f : 120.0f, y, 1480.0f, y + 54.0f), Theme::kText,
                             HAlign::Left, VAlign::Middle);
        if (row == row_)
            DrawRing(renderer_, rect, 27.0f, 1.0f);
    }

    void Draw() {
        const Text title = page_ == Page::Remove        ? Text::AccountRemoveQuestion
                           : page_ == Page::DeleteSaves ? Text::AccountDeleteQuestion
                           : page_ == Page::Colors      ? Text::AccountBackground
                           : page_ == Page::Picture     ? Text::ProfilePicture
                           : page_ == Page::Presets     ? Text::AccountCharacter
                                                        : Text::AccountsTitle;
        renderer_.DrawString(Tr(title), Font::Heading, RectF(96, 115, 1800, 210), Theme::kText);
        if (page_ == Page::Detail || page_ == Page::Picture || page_ == Page::Remove ||
            page_ == Page::DeleteSaves)
            renderer_.DrawString(Name(account_), Font::Nav, RectF(96, 210, 1800, 270),
                                 Theme::kTextSecondary);
        if (page_ == Page::List) {
            if (!launch_)
                DrawRow(0, Tr(state_.multi ? Text::AccountsMulti : Text::AccountsSingle));
            if (state_.multi) {
                for (std::size_t i = 0; i < profiles_.GetUserCount(); ++i) {
                    auto name = Name(i);
                    if (profiles_.GetUser(i)->RawString() == state_.selected_uuid)
                        name += L" · " + std::wstring(Tr(Text::AccountSelected));
                    DrawRow(static_cast<int>(i) + (launch_ ? 0 : 1), name, &images_[i]);
                }
                if (!launch_ && profiles_.GetUserCount() < Service::Account::MAX_USERS)
                    DrawRow(static_cast<int>(profiles_.GetUserCount()) + 1, Tr(Text::AccountAdd));
            }
        } else if (page_ == Page::Picture) {
            DrawRow(0, Tr(Text::AccountCharacter));
            DrawRow(1, Tr(Text::MoruIcon));
        } else if (page_ == Page::Detail) {
            DrawRow(0, Tr(Text::AccountUse));
            DrawRow(1, Tr(Text::AccountRename));
            DrawRow(2, Tr(Text::AccountAvatar));
            DrawRow(3, Tr(profiles_.GetUserCount() > 1 ? Text::AccountRemove : Text::AccountLast));
            DrawRow(4, Tr(Text::MoruTitle));
            DrawRow(5, Tr(Text::MoruIcon));
        } else if (page_ == Page::Presets) {
            for (int i = 0; i < 8; ++i) {
                const float x = 96.0f + i * 214.0f;
                const auto tab = RectF(x, 210, x + 202, 262);
                renderer_.DrawString(Tr(kCategoryLabels[i]), Font::Body, tab, Theme::kText,
                                     HAlign::Center, VAlign::Middle);
                if (i == category_)
                    DrawRing(renderer_, tab, 26.0f, 1.0f);
            }
            const int first = std::max(0, row_ / 6 - 3) * 6;
            for (int i = first; i < std::min(first + 24, static_cast<int>(visible_.size())); ++i) {
                const float x = 440.0f + (i % 6) * 180.0f;
                const float y = 290.0f + ((i - first) / 6) * 156.0f;
                const auto rect = RectF(x, y, x + 140, y + 140);
                renderer_.FillRounded(rect, 22.0f, Theme::kTextSecondary);
                renderer_.DrawImage(presets_[visible_[i]], rect, 22.0f, 1.0f);
                if (i == row_)
                    DrawRing(renderer_, rect, 22.0f, 1.0f);
            }
            renderer_.DrawString(Tr(Text::AvatarCategoryHint), Font::Body,
                                 RectF(96, 920, 1800, 970), Theme::kTextSecondary);
        } else if (page_ == Page::Colors) {
            renderer_.DrawImage(preview_, RectF(820, 270, 1100, 550), 24.0f, 1.0f);
            renderer_.DrawString(Tr(kAvatarColorLabels[row_]), Font::Nav, RectF(96, 555, 1800, 610),
                                 Theme::kText, HAlign::Center, VAlign::Middle);
            for (int i = 0; i < static_cast<int>(Core::Constants::NXBOX_AVATAR_COLORS.size());
                 ++i) {
                const auto& color = Core::Constants::NXBOX_AVATAR_COLORS[i];
                const float x = 470.0f + (i % 10) * 98.0f;
                const float y = 630.0f + (i / 10) * 80.0f;
                const auto rect = RectF(x, y, x + 64, y + 64);
                const auto tint = [](const std::array<u8, 3>& rgb) {
                    return D2D1::ColorF(rgb[0] / 255.0f, rgb[1] / 255.0f, rgb[2] / 255.0f);
                };
                if (color.rgb == color.end) {
                    renderer_.FillRounded(rect, 32.0f, tint(color.rgb));
                } else {
                    const D2D1_GRADIENT_STOP stops[] = {{0.0f, tint(color.rgb)},
                                                        {1.0f, tint(color.end)}};
                    renderer_.FillGradient(rect, 32.0f, stops, 2, D2D1::Point2F(x, y),
                                           D2D1::Point2F(x, y + 64));
                }
                renderer_.StrokeRounded(rect, 32.0f, 1.0f, Theme::kTextTertiary);
                if (i == row_)
                    DrawRing(renderer_, rect, 32.0f, 1.0f);
            }
        } else {
            DrawRow(0, Tr(page_ == Page::Remove ? Text::AccountsNo : Text::AccountKeepSaves));
            DrawRow(1, Tr(page_ == Page::Remove ? Text::AccountRemove : Text::AccountDeleteSaves));
        }
        DrawHints(renderer_,
                  {{Theme::kButtonA, L"A",
                    Tr(page_ == Page::Colors ? Text::AccountConfirmAvatar : Text::HintSelect)},
                   {Theme::kButtonB, L"B", Tr(Text::HintBack)}});
        if (failed_)
            DrawToastCapsule(renderer_, Tr(Text::AccountFailed), 1.0f);
        if (entry_)
            entry_->Draw(renderer_, Clock::now());
    }

    Renderer& renderer_;
    CoreWindow window_;
    Input& input_;
    std::filesystem::path local_state_;
    Accounts::State state_;
    Manager profiles_;
    std::array<Image, Service::Account::MAX_USERS> images_;
    std::array<Image, Core::Constants::NXBOX_CHARACTERS.size()> presets_;
    Image preview_;
    Pixels composite_;
    std::optional<Service::Mii::StoreData> moru_;
    int character_ = 0;
    std::unique_ptr<TextEntry> entry_;
    Page page_ = Page::List;
    int row_ = 0;
    std::size_t account_ = 0;
    std::vector<std::size_t> visible_;
    int category_ = 0;
    bool picture_only_ = false;
    bool launch_ = false, adding_ = false, failed_ = false;
    bool closed_ = false, done_ = false, accepted_ = false;
};
} // namespace

bool RunAccountsScreen(Renderer& renderer, const CoreWindow& window, Input& input,
                       const std::filesystem::path& local_state, bool picture_only) {
    AccountsScreen screen(renderer, window, input, local_state, false, picture_only);
    screen.Run();
    return screen.Closed();
}

bool PrepareAccounts(const CoreWindow& window, const std::filesystem::path& local_state,
                     bool choose_for_launch) {
    const auto state = Accounts::Load(local_state);
    if (!Accounts::NeedsPicker(state.initialized, state.multi, choose_for_launch)) {
        Accounts::Apply(local_state);
        return true;
    }
    Renderer renderer;
    renderer.Initialize(window);
    Input input(window);
    AccountsScreen screen(renderer, window, input, local_state, true);
    return screen.Run();
}
} // namespace EdenXbox::Ui
