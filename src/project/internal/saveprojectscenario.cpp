/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore
 * Music Composition & Notation
 *
 * Copyright (C) 2022 MuseScore BVBA and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "saveprojectscenario.h"

#include "engraving/infrastructure/mscio.h"

using namespace mu;
using namespace mu::framework;
using namespace mu::project;

constexpr int RET_CODE_CHANGE_SAVE_LOCATION_TYPE = 1234;

RetVal<SaveLocation> SaveProjectScenario::askSaveLocation(INotationProjectPtr project, SaveMode mode,
                                                          SaveLocationType preselectedType) const
{
    SaveLocationType type = preselectedType;

    if (type == SaveLocationType::Undefined) {
        RetVal<SaveLocationType> askedType = saveLocationType();
        if (!askedType.ret) {
            return askedType.ret;
        }

        type = askedType.val;
    }

    IF_ASSERT_FAILED(type != SaveLocationType::Undefined) {
        return make_ret(Ret::Code::UnknownError);
    }

    // The user may switch between Local and Cloud as often as they want
    for (;;) {
        configuration()->setLastUsedSaveLocationType(type);

        switch (type) {
        case SaveLocationType::Undefined:
            return make_ret(Ret::Code::UnknownError);

        case SaveLocationType::Local: {
            RetVal<io::path_t> path = askLocalPath(project, mode);
            switch (path.ret.code()) {
            case int(Ret::Code::Ok): {
                return RetVal<SaveLocation>::make_ok(SaveLocation(path.val));
            }
            case RET_CODE_CHANGE_SAVE_LOCATION_TYPE:
                type = SaveLocationType::Cloud;
                continue;
            default:
                return path.ret;
            }
        }

        case SaveLocationType::Cloud: {
            RetVal<CloudProjectInfo> info = askCloudLocation(project, mode);
            switch (info.ret.code()) {
            case int(Ret::Code::Ok):
                return RetVal<SaveLocation>::make_ok(SaveLocation(info.val));
            case RET_CODE_CHANGE_SAVE_LOCATION_TYPE:
                type = SaveLocationType::Local;
                continue;
            default:
                return info.ret;
            }
        }
        }
    }
}

RetVal<io::path_t> SaveProjectScenario::askLocalPath(INotationProjectPtr project, SaveMode saveMode) const
{
    QString dialogTitle = qtrc("project/save", "Save score");
    std::string filenameAddition;

    if (saveMode == SaveMode::SaveCopy) {
        //: used to form a filename suggestion, like "originalFile - copy"
        filenameAddition = " - " + trc("project/save", "copy", "a copy of a file");
    } else if (saveMode == SaveMode::SaveSelection) {
        //: used to form a filename suggestion, like "originalFile - selection"
        filenameAddition = " - " + trc("project/save", "selection");
    }

    io::path_t defaultPath = configuration()->defaultSavingFilePath(project, filenameAddition);

    QStringList filter {
        qtrc("project", "MuseScore file") + " (*.mscz)",
        qtrc("project", "Uncompressed MuseScore folder (experimental)")
#ifdef Q_OS_MAC
        + " (*)"
#else
        + " (*.)"
#endif
    };

    io::path_t selectedPath = interactive()->selectSavingFile(dialogTitle, defaultPath, filter.join(";;"));

    if (selectedPath.empty()) {
        return make_ret(Ret::Code::Cancel);
    }

    if (!engraving::isMuseScoreFile(io::suffix(selectedPath))) {
        // Then it must be that the user is trying to save a mscx file.
        // At the selected path, a folder will be created,
        // and inside the folder, a mscx file will be created.
        // We should return the path to the mscx file.
        selectedPath = selectedPath.appendingComponent(io::filename(selectedPath)).appendingSuffix(engraving::MSCX);
    }

    configuration()->setLastSavedProjectsPath(io::dirpath(selectedPath));

    return RetVal<io::path_t>::make_ok(selectedPath);
}

RetVal<SaveLocationType> SaveProjectScenario::saveLocationType() const
{
    bool shouldAsk = configuration()->shouldAskSaveLocationType();
    SaveLocationType lastUsed = configuration()->lastUsedSaveLocationType();
    if (!shouldAsk && lastUsed != SaveLocationType::Undefined) {
        return RetVal<SaveLocationType>::make_ok(lastUsed);
    }

    return askSaveLocationType();
}

RetVal<SaveLocationType> SaveProjectScenario::askSaveLocationType() const
{
    UriQuery query("musescore://project/asksavelocationtype");
    bool shouldAsk = configuration()->shouldAskSaveLocationType();
    query.addParam("askAgain", Val(shouldAsk));

    RetVal<Val> rv = interactive()->open(query);
    if (!rv.ret) {
        return rv.ret;
    }

    QVariantMap vals = rv.val.toQVariant().toMap();

    bool askAgain = vals["askAgain"].toBool();
    configuration()->setShouldAskSaveLocationType(askAgain);

    SaveLocationType type = static_cast<SaveLocationType>(vals["saveLocationType"].toInt());
    return RetVal<SaveLocationType>::make_ok(type);
}

RetVal<CloudProjectInfo> SaveProjectScenario::askCloudLocation(INotationProjectPtr project, SaveMode mode) const
{
    return doAskCloudLocation(project, mode, false);
}

RetVal<CloudProjectInfo> SaveProjectScenario::askPublishLocation(INotationProjectPtr project) const
{
    return doAskCloudLocation(project, SaveMode::Save, true);
}

RetVal<CloudProjectInfo> SaveProjectScenario::doAskCloudLocation(INotationProjectPtr project, SaveMode mode, bool isPublish) const
{
    if (!authorizationService()->userAuthorized().val) {
        Ret ret = authorizationService()->requireAuthorization(
            trc("project/save", "Login or create a free account on musescore.com to save this score to the cloud."));
        if (!ret) {
            return ret;
        }
    }

    // TODO(save-to-cloud): better name?
    QString defaultName = project->displayName();
    CloudProjectVisibility defaultVisibility = isPublish ? CloudProjectVisibility::Public : CloudProjectVisibility::Private;

    UriQuery query("musescore://project/savetocloud");
    query.addParam("isPublish", Val(isPublish));
    query.addParam("name", Val(defaultName));
    query.addParam("visibility", Val(defaultVisibility));

    RetVal<Val> rv = interactive()->open(query);
    if (!rv.ret) {
        return rv.ret;
    }

    QVariantMap vals = rv.val.toQVariant().toMap();
    using Response = QMLSaveToCloudResponse::SaveToCloudResponse;
    auto response = static_cast<Response>(vals["response"].toInt());
    switch (response) {
    case Response::Cancel:
        return make_ret(Ret::Code::Cancel);
    case Response::SaveLocallyInstead:
        return Ret(RET_CODE_CHANGE_SAVE_LOCATION_TYPE);
    case Response::Ok:
        break;
    }

    CloudProjectInfo result;
    result.name = vals["name"].toString();
    result.visibility = static_cast<CloudProjectVisibility>(vals["visibility"].toInt());

    if (!warnBeforePublishing(project, result.visibility, isPublish)) {
        return make_ret(Ret::Code::Cancel);
    }

    if (mode == SaveMode::Save) {
        result.sourceUrl = project->cloudInfo().sourceUrl;
    }

    return RetVal<CloudProjectInfo>::make_ok(result);
}

bool SaveProjectScenario::warnBeforePublishing(INotationProjectPtr project, CloudProjectVisibility visibility, bool isPublish) const
{
    if (!isPublish && !configuration()->shouldWarnBeforeSavingPublicly()) {
        return true;
    }

    IInteractive::ButtonDatas buttons = {
        IInteractive::ButtonData(IInteractive::Button::Cancel, trc("global", "Cancel")),
        IInteractive::ButtonData(IInteractive::Button::Ok, trc("project/save", "Publish"), true)
    };

    IInteractive::Result result;

    if (isPublish) {
        if (!project->isCloudProject()) {
            return true;
        }

        std::string title = trc("project/save", "Publish changes online?");
        std::string message = trc("project/save", "Your saved changes will be publicly visible. We will also "
                                                  "need to generate a new MP3 for public playback.");

        result = interactive()->warning(title, message, buttons, int(IInteractive::Button::Ok), IInteractive::Option::WithIcon);
    } else if (visibility == CloudProjectVisibility::Public) {
        result = interactive()->warning(
            trc("project/save", "Publish this score online?"),
            trc("project/save", "All saved changes will be publicly visible on MuseScore.com. "
                                "If you want to make frequent changes, we recommend saving this "
                                "score privately until you’re ready to share it to the world. "),
            buttons,
            int(IInteractive::Button::Ok),
            IInteractive::Option::WithIcon | IInteractive::Option::WithDontShowAgainCheckBox);
    } else {
        return true;
    }

    bool publish = result.standardButton() == IInteractive::Button::Ok;
    if (publish && !result.showAgain()) {
        configuration()->setShouldWarnBeforeSavingPublicly(false);
    }

    return publish;
}
