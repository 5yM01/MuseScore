/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore BVBA and others
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

#include "wavewriter.h"

#include "log.h"

using namespace mu::iex::audioexport;
using namespace mu::framework;

mu::Ret WaveWriter::write(notation::INotationPtr, QIODevice& destinationDevice, const Options&)
{
    static const audio::SoundTrackFormat format {
        audio::SoundTrackType::WAV,
        static_cast<audio::sample_rate_t>(configuration()->exportSampleRate()),
        2 /* audioChannelsNumber */,
        0 /* bitRate */
    };

    doWriteAndWait(destinationDevice, format);

    return make_ret(Ret::Code::Ok);
}
