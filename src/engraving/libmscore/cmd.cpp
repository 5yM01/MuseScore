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

/**
 \file
 Handling of several GUI commands.
*/

#include <assert.h>

#include "translation.h"

#include "infrastructure/messagebox.h"
#include "style/style.h"
#include "rw/xml.h"

#include "accidental.h"
#include "articulation.h"
#include "barline.h"
#include "box.h"
#include "chord.h"
#include "clef.h"
#include "drumset.h"
#include "dynamic.h"
#include "factory.h"
#include "hairpin.h"
#include "harmony.h"
#include "key.h"
#include "linkedobjects.h"
#include "lyrics.h"
#include "masterscore.h"
#include "measure.h"
#include "measurerepeat.h"
#include "mscore.h"
#include "mscoreview.h"
#include "navigate.h"
#include "note.h"
#include "page.h"
#include "part.h"
#include "pitchspelling.h"
#include "rehearsalmark.h"
#include "rest.h"
#include "score.h"
#include "segment.h"
#include "sig.h"
#include "slur.h"
#include "staff.h"
#include "stafftype.h"
#include "stem.h"
#include "stringdata.h"
#include "system.h"
#include "tie.h"
#include "timesig.h"
#include "tremolo.h"
#include "tuplet.h"
#include "types.h"
#include "undo.h"
#include "utils.h"

#include "log.h"

using namespace mu;
using namespace mu::io;
using namespace mu::engraving;

namespace mu::engraving {
static UndoMacro::ChangesInfo changesInfo(const UndoStack* stack)
{
    IF_ASSERT_FAILED(stack) {
        static UndoMacro::ChangesInfo empty;
        return empty;
    }

    const UndoMacro* actualMacro = stack->current();

    if (!actualMacro) {
        actualMacro = stack->last();
    }

    if (!actualMacro) {
        static UndoMacro::ChangesInfo empty;
        return empty;
    }

    return actualMacro->changesInfo();
}

static std::pair<int, int> changedTicksRange(const CmdState& cmdState, const std::vector<const EngravingItem*>& changedItems)
{
    int startTick = cmdState.startTick().ticks();
    int endTick = cmdState.endTick().ticks();

    for (const EngravingItem* element : changedItems) {
        int tick = element->tick().ticks();

        if (startTick > tick) {
            startTick = tick;
        }

        if (endTick < tick) {
            endTick = tick;
        }
    }

    return { startTick, endTick };
}

//---------------------------------------------------------
//   reset
//---------------------------------------------------------

void CmdState::reset()
{
    layoutFlags         = LayoutFlag::NO_FLAGS;
    _updateMode         = UpdateMode::DoNothing;
    _startTick          = Fraction(-1, 1);
    _endTick            = Fraction(-1, 1);

    _startStaff = mu::nidx;
    _endStaff = mu::nidx;
    _el = nullptr;
    _oneElement = true;
    _mb = nullptr;
    _oneMeasureBase = true;
    _locked = false;
}

//---------------------------------------------------------
//   setTick
//---------------------------------------------------------

void CmdState::setTick(const Fraction& t)
{
    if (_locked) {
        return;
    }

    if (_startTick == Fraction(-1, 1) || t < _startTick) {
        _startTick = t;
    }
    if (_endTick == Fraction(-1, 1) || t > _endTick) {
        _endTick = t;
    }
    setUpdateMode(UpdateMode::Layout);
}

//---------------------------------------------------------
//   setStaff
//---------------------------------------------------------

void CmdState::setStaff(staff_idx_t st)
{
    if (_locked || st == mu::nidx) {
        return;
    }

    if (_startStaff == mu::nidx || st < _startStaff) {
        _startStaff = st;
    }
    if (_endStaff == mu::nidx || st > _endStaff) {
        _endStaff = st;
    }
}

//---------------------------------------------------------
//   setMeasureBase
//---------------------------------------------------------

void CmdState::setMeasureBase(const MeasureBase* mb)
{
    if (!mb || _mb == mb || _locked) {
        return;
    }

    _oneMeasureBase = !_mb;
    _mb = mb;
}

//---------------------------------------------------------
//   setElement
//---------------------------------------------------------

void CmdState::setElement(const EngravingItem* e)
{
    if (!e || _el == e || _locked) {
        return;
    }

    _oneElement = !_el;
    _el = e;

    if (_oneMeasureBase) {
        setMeasureBase(e->findMeasureBase());
    }
}

//---------------------------------------------------------
//   unsetElement
//---------------------------------------------------------

void CmdState::unsetElement(const EngravingItem* e)
{
    if (_el == e) {
        _el = nullptr;
    }
    if (_mb == e) {
        _mb = nullptr;
    }
}

//---------------------------------------------------------
//   element
//---------------------------------------------------------

const EngravingItem* CmdState::element() const
{
    if (_oneElement) {
        return _el;
    }
    if (_oneMeasureBase) {
        return _mb;
    }
    return nullptr;
}

//---------------------------------------------------------
//   setUpdateMode
//---------------------------------------------------------

void CmdState::_setUpdateMode(UpdateMode m)
{
    _updateMode = m;
}

void CmdState::setUpdateMode(UpdateMode m)
{
    if (int(m) > int(_updateMode)) {
        _setUpdateMode(m);
    }
}

//---------------------------------------------------------
//   startCmd
///   Start a GUI command by clearing the redraw area
///   and starting a user-visible undo.
//---------------------------------------------------------

void Score::startCmd()
{
    if (MScore::debugMode) {
        LOGD("===startCmd()");
    }

    MScore::setError(MsError::MS_NO_ERROR);

    cmdState().reset();

    // Start collecting low-level undo operations for a
    // user-visible undo action.
    if (undoStack()->active()) {
        LOGD("Score::startCmd(): cmd already active");
        return;
    }
    undoStack()->beginMacro(this);
}

//---------------------------------------------------------
//   undoRedo
//---------------------------------------------------------

void Score::undoRedo(bool undo, EditData* ed)
{
    if (readOnly()) {
        return;
    }

    //! NOTE: the order of operations is very important here
    //! 1. for the undo operation, the list of changed elements is available before undo()
    //! 2. for the redo operation, the list of changed elements will be available after redo()
    UndoMacro::ChangesInfo changes = changesInfo(undoStack());

    cmdState().reset();
    if (undo) {
        undoStack()->undo(ed);
    } else {
        undoStack()->redo(ed);
    }
    update(false);
    masterScore()->setPlaylistDirty();    // TODO: flag all individual operations
    updateSelection();

    ScoreChangesRange range = changesRange();

    if (range.changedTypes.empty()) {
        range.changedTypes = std::move(changes.changedObjectTypes);
    }

    if (range.changedPropertyIdSet.empty()) {
        range.changedPropertyIdSet = std::move(changes.changedPropertyIdSet);
    }

    if (range.changedStyleIdSet.empty()) {
        range.changedStyleIdSet = std::move(changes.changedStyleIdSet);
    }

    if (range.isValid()) {
        m_changesRangeChannel.send(range);
    }
}

//---------------------------------------------------------
//   endCmd
///   End a GUI command by (if \a undo) ending a user-visible undo
///   and (always) updating the redraw area.
//---------------------------------------------------------

void Score::endCmd(bool rollback)
{
    if (!undoStack()->active()) {
        LOGW() << "no command active";
        update();
        return;
    }

    if (readOnly() || MScore::_error != MsError::MS_NO_ERROR) {
        rollback = true;
    }

    if (rollback) {
        undoStack()->current()->unwind();
    }

    update(false);

    ScoreChangesRange range = changesRange();

    LOGD() << "Undo stack current macro child count: " << undoStack()->current()->childCount();

    const bool noUndo = undoStack()->current()->empty(); // nothing to undo?
    undoStack()->endMacro(noUndo);

    if (dirty()) {
        masterScore()->setPlaylistDirty(); // TODO: flag individual operations
        masterScore()->setAutosaveDirty(true);
    }

    cmdState().reset();

    if (!rollback && range.isValid()) {
        m_changesRangeChannel.send(range);
    }
}

mu::async::Channel<ScoreChangesRange> Score::changesChannel() const
{
    return m_changesRangeChannel;
}

ScoreChangesRange Score::changesRange() const
{
    const CmdState& cmdState = score()->cmdState();
    UndoMacro::ChangesInfo changes = changesInfo(undoStack());
    auto ticksRange = changedTicksRange(cmdState, changes.changedItems);

    return { ticksRange.first, ticksRange.second,
             cmdState.startStaff(), cmdState.endStaff(),
             changes.changedObjectTypes, changes.changedPropertyIdSet, changes.changedStyleIdSet };
}

#ifndef NDEBUG
//---------------------------------------------------------
//   CmdState::dump
//---------------------------------------------------------

void CmdState::dump()
{
    LOGD("CmdState: mode %d %d-%d", int(_updateMode), _startTick.ticks(), _endTick.ticks());
    // bool _excerptsChanged     { false };
    // bool _instrumentsChanged  { false };
}

#endif

//---------------------------------------------------------
//   update
//    layout & update
//---------------------------------------------------------

void Score::update(bool resetCmdState)
{
    TRACEFUNC;

    bool updateAll = false;
    {
        MasterScore* ms = masterScore();
        CmdState& cs = ms->cmdState();
        ms->deletePostponed();
        if (cs.layoutRange()) {
            for (Score* s : ms->scoreList()) {
                s->doLayoutRange(cs.startTick(), cs.endTick());
            }
            updateAll = true;
        }
    }

    {
        MasterScore* ms = masterScore();
        CmdState& cs = ms->cmdState();
        if (updateAll || cs.updateAll()) {
            for (Score* s : scoreList()) {
                for (MuseScoreView* v : s->viewer) {
                    v->updateAll();
                }
            }
        } else if (cs.updateRange()) {
            // updateRange updates only current score
            double d = spatium() * .5;
            _updateState.refresh.adjust(-d, -d, 2 * d, 2 * d);
            for (MuseScoreView* v : viewer) {
                v->dataChanged(_updateState.refresh);
            }
            _updateState.refresh = RectF();
        }
        const InputState& is = inputState();
        if (is.noteEntryMode() && is.segment()) {
            setPlayPos(is.segment()->tick());
        }
        if (playlistDirty()) {
            masterScore()->setPlaylistClean();
        }
        if (resetCmdState) {
            cs.reset();
        }
    }

    if (_selection.isRange() && !_selection.isLocked()) {
        _selection.updateSelectedElements();
    }
}

//---------------------------------------------------------
//   deletePostponed
//---------------------------------------------------------

void Score::deletePostponed()
{
    for (EngravingObject* e : _updateState._deleteList) {
        if (e->isSystem()) {
            System* s = toSystem(e);
            std::list<SpannerSegment*> spanners = s->spannerSegments();
            for (SpannerSegment* ss : spanners) {
                if (ss->system() == s) {
                    ss->setSystem(0);
                }
            }
        }
    }
    DeleteAll(_updateState._deleteList);
    _updateState._deleteList.clear();
}

//---------------------------------------------------------
//   cmdAddSpanner
//   drop VOLTA, OTTAVA, TRILL, PEDAL, DYNAMIC
//        HAIRPIN, LET_RING, VIBRATO and TEXTLINE
//---------------------------------------------------------

void Score::cmdAddSpanner(Spanner* spanner, const PointF& pos, bool systemStavesOnly)
{
    staff_idx_t staffIdx = spanner->staffIdx();
    Segment* segment;
    MeasureBase* mb = pos2measure(pos, &staffIdx, 0, &segment, 0);
    if (systemStavesOnly) {
        staffIdx = 0;
    }
    // ignore if we do not have a measure
    if (mb == 0 || mb->type() != ElementType::MEASURE) {
        LOGD("cmdAddSpanner: cannot put object here");
        delete spanner;
        return;
    }

    // all spanners live in voice 0 (except slurs/ties)
    track_idx_t track = staffIdx == mu::nidx ? mu::nidx : staffIdx * VOICES;

    spanner->setTrack(track);
    spanner->setTrack2(track);

    if (spanner->anchor() == Spanner::Anchor::SEGMENT) {
        spanner->setTick(segment->tick());
        Fraction lastTick = lastMeasure()->tick() + lastMeasure()->ticks();
        Fraction tick2 = std::min(segment->measure()->tick() + segment->measure()->ticks(), lastTick);
        spanner->setTick2(tick2);
    } else {      // Anchor::MEASURE, Anchor::CHORD, Anchor::NOTE
        Measure* m = toMeasure(mb);
        RectF b(m->canvasBoundingRect());

        if (pos.x() >= (b.x() + b.width() * .5) && m != lastMeasureMM() && m->nextMeasure()->system() == m->system()) {
            m = m->nextMeasure();
        }
        spanner->setTick(m->tick());
        spanner->setTick2(m->endTick());
    }
    spanner->eraseSpannerSegments();

    bool ctrlModifier = isSystemTextLine(spanner) && !systemStavesOnly;
    undoAddElement(spanner, true /*addToLinkedStaves*/, ctrlModifier);
    select(spanner, SelectType::SINGLE, 0);
}

//---------------------------------------------------------
//   cmdAddSpanner
//    used when applying a spanner to a selection
//---------------------------------------------------------

void Score::cmdAddSpanner(Spanner* spanner, staff_idx_t staffIdx, Segment* startSegment, Segment* endSegment)
{
    track_idx_t track = staffIdx * VOICES;
    spanner->setTrack(track);
    spanner->setTrack2(track);
    for (auto ss : spanner->spannerSegments()) {
        ss->setTrack(track);
    }
    spanner->setTick(startSegment->tick());
    Fraction tick2;
    if (!endSegment) {
        tick2 = lastSegment()->tick();
    } else if (endSegment == startSegment) {
        tick2 = startSegment->measure()->last()->tick();
    } else {
        tick2 = endSegment->tick();
    }
    spanner->setTick2(tick2);
    undoAddElement(spanner);
}

//---------------------------------------------------------
//   expandVoice
//    fills gaps in voice with rests,
//    from previous cr (or beginning of measure) to next cr (or end of measure)
//---------------------------------------------------------

void Score::expandVoice(Segment* s, track_idx_t track)
{
    if (!s) {
        LOGD("expand voice: no segment");
        return;
    }
    if (s->element(track)) {
        return;
    }

    // find previous segment with cr in this track
    Segment* ps;
    for (ps = s; ps; ps = ps->prev(SegmentType::ChordRest)) {
        if (ps->element(track)) {
            break;
        }
    }
    if (ps) {
        ChordRest* cr = toChordRest(ps->element(track));
        Fraction tick = cr->tick() + cr->actualTicks();
        if (tick > s->tick()) {
            // previous cr extends past current segment
            LOGD("expandVoice: cannot insert element here");
            return;
        }
        if (cr->isChord()) {
            // previous cr ends on or before current segment
            // for chords, move ps to just after cr ends
            // so we can fill any gap that might exist
            // but don't move ps if previous cr is a rest
            // this will be combined with any new rests needed to fill up to s->tick() below
            ps = ps->measure()->undoGetSegment(SegmentType::ChordRest, tick);
        }
    }
    //
    // fill up to s->tick() with rests
    //
    Measure* m = s->measure();
    Fraction stick  = ps ? ps->tick() : m->tick();
    Fraction ticks  = s->tick() - stick;
    if (ticks.isNotZero()) {
        setRest(stick, track, ticks, false, 0);
    }

    //
    // fill from s->tick() until next chord/rest in measure
    //
    Segment* ns;
    for (ns = s->next(SegmentType::ChordRest); ns; ns = ns->next(SegmentType::ChordRest)) {
        if (ns->element(track)) {
            break;
        }
    }
    ticks  = ns ? (ns->tick() - s->tick()) : (m->ticks() - s->rtick());
    if (ticks == m->ticks()) {
        addRest(s, track, TDuration(DurationType::V_MEASURE), 0);
    } else {
        setRest(s->tick(), track, ticks, false, 0);
    }
}

void Score::expandVoice()
{
    Segment* s = _is.segment();
    track_idx_t track = _is.track();
    expandVoice(s, track);
}

//---------------------------------------------------------
//   addInterval
//---------------------------------------------------------

void Score::addInterval(int val, const std::vector<Note*>& nl)
{
    for (Note* on : nl) {
        Chord* chord = on->chord();
        Note* note = Factory::createNote(chord);
        note->setParent(chord);
        note->setTrack(chord->track());
        int valTmp = val < 0 ? val + 1 : val - 1;

        int npitch;
        int ntpc1;
        int ntpc2;
        bool accidental = _is.noteEntryMode() && _is.accidentalType() != AccidentalType::NONE;
        bool forceAccidental = false;
        if (abs(valTmp) != 7 || accidental) {
            int line      = on->line() - valTmp;
            Fraction tick      = chord->tick();
            Staff* estaff = staff(on->staffIdx() + chord->staffMove());
            ClefType clef = estaff->clef(tick);
            Key key       = estaff->key(tick);
            int ntpc;
            if (accidental) {
                AccidentalVal acci = Accidental::subtype2value(_is.accidentalType());
                int step = absStep(line, clef);
                int octave = step / 7;
                npitch = step2pitch(step) + octave * 12 + int(acci);
                forceAccidental = (npitch == line2pitch(line, clef, key));
                ntpc = step2tpc(step % 7, acci);
            } else {
                npitch = line2pitch(line, clef, key);
                ntpc = pitch2tpc(npitch, key, Prefer::NEAREST);
            }

            Interval v = on->part()->instrument(tick)->transpose();
            if (v.isZero()) {
                ntpc1 = ntpc2 = ntpc;
            } else {
                if (styleB(Sid::concertPitch)) {
                    v.flip();
                    ntpc1 = ntpc;
                    ntpc2 = mu::engraving::transposeTpc(ntpc, v, true);
                } else {
                    npitch += v.chromatic;
                    ntpc2 = ntpc;
                    ntpc1 = mu::engraving::transposeTpc(ntpc, v, true);
                }
            }
        } else {   //special case for octave
            Interval interval(7, 12);
            if (val < 0) {
                interval.flip();
            }
            transposeInterval(on->pitch(), on->tpc(), &npitch, &ntpc1, interval, false);
            ntpc1 = on->tpc1();
            ntpc2 = on->tpc2();
        }
        if (npitch < 0 || npitch > 127) {
            delete note;
            endCmd();
            return;
        }
        note->setPitch(npitch, ntpc1, ntpc2);

        undoAddElement(note);
        if (forceAccidental) {
            Accidental* a = Factory::createAccidental(note);
            a->setAccidentalType(_is.accidentalType());
            a->setRole(AccidentalRole::USER);
            a->setParent(note);
            undoAddElement(a);
        }
        setPlayNote(true);

        select(note, SelectType::SINGLE, 0);
    }
    if (_is.noteEntryMode()) {
        _is.setAccidentalType(AccidentalType::NONE);
    }
    _is.moveToNextInputPos();
}

//---------------------------------------------------------
//   setGraceNote
///   Create a grace note in front of a normal note.
///   \arg ch is the chord of the normal note
///   \arg pitch is the pitch of the grace note
///   \arg is the grace note type
///   \len is the visual duration of the grace note (1/16 or 1/32)
//---------------------------------------------------------

Note* Score::setGraceNote(Chord* ch, int pitch, NoteType type, int len)
{
    Chord* chord = Factory::createChord(this->dummy()->segment());
    Note* note = Factory::createNote(chord);

    // allow grace notes to be added to other grace notes
    // by really adding to parent chord
    if (ch->noteType() != NoteType::NORMAL) {
        ch = toChord(ch->explicitParent());
    }

    chord->setTrack(ch->track());
    chord->setParent(ch);
    chord->add(note);

    note->setPitch(pitch);
    // find corresponding note within chord and use its tpc information
    for (Note* n : ch->notes()) {
        if (n->pitch() == pitch) {
            note->setTpc1(n->tpc1());
            note->setTpc2(n->tpc2());
            break;
        }
    }
    // note with same pitch not found, derive tpc from pitch / key
    if (!tpcIsValid(note->tpc1()) || !tpcIsValid(note->tpc2())) {
        note->setTpcFromPitch();
    }

    TDuration d;
    d.setVal(len);
    chord->setDurationType(d);
    chord->setTicks(d.fraction());
    chord->setNoteType(type);
    chord->setMag(ch->staff()->staffMag(chord->tick()) * styleD(Sid::graceNoteMag));

    undoAddElement(chord);
    select(note, SelectType::SINGLE, 0);
    return note;
}

//---------------------------------------------------------
//   createCRSequence
//    Create a rest or chord of len f.
//    If f is not a basic len, create several rests or
//    tied chords.
//
//    f     total len of ChordRest
//    cr    prototype CR
//    tick  start position in measure
//---------------------------------------------------------

void Score::createCRSequence(const Fraction& f, ChordRest* cr, const Fraction& t)
{
    Fraction tick(t);
    Measure* measure = cr->measure();
    ChordRest* ocr = 0;
    for (TDuration d : toDurationList(f, true)) {
        ChordRest* ncr = toChordRest(cr->clone());
        ncr->setDurationType(d);
        ncr->setTicks(d.fraction());
        undoAddCR(ncr, measure, measure->tick() + tick);
        if (cr->isChord() && ocr) {
            Chord* nc = toChord(ncr);
            Chord* oc = toChord(ocr);
            for (unsigned int i = 0; i < oc->notes().size(); ++i) {
                Note* on = oc->notes()[i];
                Note* nn = nc->notes()[i];
                Tie* tie = Factory::createTie(this->dummy());
                tie->setStartNote(on);
                tie->setEndNote(nn);
                tie->setTick(tie->startNote()->tick());
                tie->setTick2(tie->endNote()->tick());
                tie->setTrack(cr->track());
                on->setTieFor(tie);
                nn->setTieBack(tie);
                undoAddElement(tie);
            }
        }

        tick += ncr->actualTicks();
        ocr = ncr;
    }
}

//---------------------------------------------------------
//   setNoteRest
//    pitch == -1  -> set rest
//    return segment of last created note/rest
//---------------------------------------------------------

Segment* Score::setNoteRest(Segment* segment, track_idx_t track, NoteVal nval, Fraction sd, DirectionV stemDirection,
                            bool forceAccidental, const std::set<SymId>& articulationIds, bool rhythmic, InputState* externalInputState)
{
    assert(segment->segmentType() == SegmentType::ChordRest);
    InputState& is = externalInputState ? (*externalInputState) : _is;

    bool isRest   = nval.pitch == -1;
    Fraction tick = segment->tick();
    EngravingItem* nr   = nullptr;
    Tie* tie      = nullptr;
    ChordRest* cr = toChordRest(segment->element(track));
    Tuplet* tuplet = cr && cr->tuplet() && sd <= cr->tuplet()->ticks() ? cr->tuplet() : nullptr;
    Measure* measure = nullptr;
    bool targetIsRest = cr && cr->isRest();
    for (;;) {
        if (track % VOICES) {
            expandVoice(segment, track);
        }
        if (targetIsRest && !cr->isRest()) {
            undoRemoveElement(cr);
            segment = addRest(segment, track, cr->ticks(), cr->tuplet())->segment();
        }
        // the returned gap ends at the measure boundary or at tuplet end
        Fraction dd = makeGap(segment, track, sd, tuplet);

        if (dd.isZero()) {
            LOGD("cannot get gap at %d type: %d/%d", tick.ticks(), sd.numerator(),
                 sd.denominator());
            break;
        }

        measure = segment->measure();
        std::vector<TDuration> dl;
        if (rhythmic) {
            dl = toRhythmicDurationList(dd, isRest, segment->rtick(), sigmap()->timesig(tick).nominal(), measure, 1);
        } else {
            dl = toDurationList(dd, true);
        }
        size_t n = dl.size();
        for (size_t i = 0; i < n; ++i) {
            const TDuration& d = dl[i];
            ChordRest* ncr = nullptr;
            Note* note = nullptr;
            Tie* addTie = nullptr;
            if (isRest) {
                nr = ncr = Factory::createRest(this->dummy()->segment());
                nr->setTrack(track);
                ncr->setDurationType(d);
                ncr->setTicks(d == DurationType::V_MEASURE ? measure->ticks() : d.fraction());
            } else {
                nr = note = Factory::createNote(this->dummy()->chord());

                if (tie) {
                    tie->setEndNote(note);
                    tie->setTick2(tie->endNote()->tick());
                    note->setTieBack(tie);
                    addTie = tie;
                }
                Chord* chord = Factory::createChord(this->dummy()->segment());
                chord->setTrack(track);
                chord->setDurationType(d);
                chord->setTicks(d.fraction());
                chord->setStemDirection(stemDirection);
                chord->add(note);
                note->setNval(nval, tick);
                if (forceAccidental) {
                    int tpc = styleB(Sid::concertPitch) ? nval.tpc1 : nval.tpc2;
                    AccidentalVal alter = tpc2alter(tpc);
                    AccidentalType at = Accidental::value2subtype(alter);
                    Accidental* a = Factory::createAccidental(note);
                    a->setAccidentalType(at);
                    a->setRole(AccidentalRole::USER);
                    note->add(a);
                }

                ncr = chord;
                if (i + 1 < n) {
                    tie = Factory::createTie(this->dummy());
                    tie->setStartNote(note);
                    tie->setTick(tie->startNote()->tick());
                    tie->setTrack(track);
                    note->setTieFor(tie);
                }
            }
            if (tuplet && sd <= tuplet->ticks()) {
                ncr->setTuplet(tuplet);
            }
            tuplet = 0;
            undoAddCR(ncr, measure, tick);
            if (addTie) {
                undoAddElement(addTie);
            }
            setPlayNote(true);
            segment = ncr->segment();
            tick += ncr->actualTicks();

            if (!articulationIds.empty()) {
                toChord(ncr)->updateArticulations(articulationIds);
            }
        }

        sd -= dd;
        if (sd.isZero()) {
            break;
        }

        Segment* nseg = tick2segment(tick, false, SegmentType::ChordRest);
        if (!nseg) {
            LOGD("reached end of score");
            break;
        }
        segment = nseg;

        cr = toChordRest(segment->element(track));

        if (!cr) {
            if (track % VOICES) {
                cr = addRest(segment, track, TDuration(DurationType::V_MEASURE), 0);
            } else {
                LOGD("no rest in voice 0");
                break;
            }
        }
        //
        //  Note does not fit on current measure, create Tie to
        //  next part of note
        if (!isRest) {
            tie = Factory::createTie(this->dummy());
            tie->setStartNote((Note*)nr);
            tie->setTick(tie->startNote()->tick());
            tie->setTrack(nr->track());
            ((Note*)nr)->setTieFor(tie);
        }
    }
    if (tie) {
        connectTies();
    }
    if (nr) {
        if (is.slur() && nr->type() == ElementType::NOTE) {
            // If the start element was the same as the end element when the slur was created,
            // the end grip of the front slur segment was given an x-offset of 3.0 * spatium().
            // Now that the slur is about to be given a new end element, this should be reset.
            if (is.slur()->endElement() == is.slur()->startElement()) {
                is.slur()->frontSegment()->reset();
            }
            //
            // extend slur
            //
            Chord* chord = toNote(nr)->chord();
            is.slur()->undoChangeProperty(Pid::SPANNER_TICKS, chord->tick() - is.slur()->tick());
            for (EngravingObject* se : is.slur()->linkList()) {
                Slur* slur = toSlur(se);
                for (EngravingObject* ee : chord->linkList()) {
                    EngravingItem* e = static_cast<EngravingItem*>(ee);
                    if (e->score() == slur->score() && e->track() == slur->track2()) {
                        slur->score()->undo(new ChangeSpannerElements(slur, slur->startElement(), e));
                        break;
                    }
                }
            }
        }
        if (externalInputState) {
            is.setTrack(nr->track());
            cr = nr->isRest() ? toChordRest(nr) : toNote(nr)->chord();
            is.setLastSegment(is.segment());
            is.setSegment(cr->segment());
        } else {
            select(nr, SelectType::SINGLE, 0);
        }
    }
    return segment;
}

//---------------------------------------------------------
//   makeGap
//    make time gap at tick by removing/shortening
//    chord/rest
//
//    if keepChord, the chord at tick is not removed
//
//    gap does not exceed measure or scope of tuplet
//
//    return size of actual gap
//---------------------------------------------------------

Fraction Score::makeGap(Segment* segment, track_idx_t track, const Fraction& _sd, Tuplet* tuplet, bool keepChord)
{
    assert(_sd.numerator());

    Measure* measure = segment->measure();
    Fraction accumulated;
    Fraction sd = _sd;

    //
    // remember first segment which should
    // not be deleted (it may contain other elements we want to preserve)
    //
    Segment* firstSegment = segment;
    const Fraction firstSegmentEnd = firstSegment->tick() + firstSegment->ticks();
    Fraction nextTick = segment->tick();

    for (Segment* seg = firstSegment; seg; seg = seg->next(SegmentType::ChordRest)) {
        //
        // voices != 0 may have gaps:
        //
        ChordRest* cr = toChordRest(seg->element(track));
        if (!cr) {
            if (seg->tick() < nextTick) {
                continue;
            }
            Segment* seg1 = seg->next(SegmentType::ChordRest);
            Fraction tick2 = seg1 ? seg1->tick() : seg->measure()->tick() + seg->measure()->ticks();
            Fraction td(tick2 - seg->tick());
            if (td > sd) {
                td = sd;
            }
            accumulated += td;
            sd -= td;
            if (sd.isZero()) {
                break;
            }
            nextTick = tick2;
            continue;
        }
        if (seg->tick() > nextTick) {
            // there was a gap
            Fraction td(seg->tick() - nextTick);
            if (td > sd) {
                td = sd;
            }
            accumulated += td;
            sd -= td;
            if (sd.isZero()) {
                break;
            }
        }
        //
        // limit to tuplet level
        //
        if (tuplet) {
            bool tupletEnd = true;
            Tuplet* t = cr->tuplet();
            while (t) {
                if (cr->tuplet() == tuplet) {
                    tupletEnd = false;
                    break;
                }
                t = t->tuplet();
            }
            if (tupletEnd) {
                break;
            }
        }
        Fraction td(cr->ticks());

        // remove tremolo between 2 notes, if present
        if (cr->isChord()) {
            Chord* c = toChord(cr);
            if (c->tremolo()) {
                Tremolo* tremolo = c->tremolo();
                if (tremolo->twoNotes()) {
                    undoRemoveElement(tremolo);
                }
            }
        }
        Tuplet* ltuplet = cr->tuplet();
        if (ltuplet != tuplet) {
            //
            // Current location points to the start of a (nested)tuplet.
            // We have to remove the complete tuplet.

            // get top level tuplet
            while (ltuplet->tuplet()) {
                ltuplet = ltuplet->tuplet();
            }

            // get last segment of tuplet, drilling down to leaf nodes as necessary
            Tuplet* t = ltuplet;
            while (t->elements().back()->isTuplet()) {
                t = toTuplet(t->elements().back());
            }
            seg = toChordRest(t->elements().back())->segment();

            // now delete the full tuplet
            td = ltuplet->ticks();
            cmdDeleteTuplet(ltuplet, false);
            tuplet = 0;
        } else {
            if (seg != firstSegment || !keepChord) {
                undoRemoveElement(cr);
            }
            // even if there was a tuplet, we didn't remove it
            ltuplet = 0;
        }
        Fraction timeStretch = cr->staff()->timeStretch(cr->tick());
        nextTick += actualTicks(td, tuplet, timeStretch);
        if (sd < td) {
            //
            // we removed too much
            //
            accumulated = _sd;
            Fraction rd = td - sd;
            Fraction tick = cr->tick() + actualTicks(sd, tuplet, timeStretch);

            std::vector<TDuration> dList;
            if (tuplet || staff(track / VOICES)->isLocalTimeSignature(tick)) {
                dList = toDurationList(rd, false);
                std::reverse(dList.begin(), dList.end());
            } else {
                dList = toRhythmicDurationList(rd, true, tick - measure->tick(), sigmap()->timesig(tick).nominal(), measure, 0);
            }
            if (dList.empty()) {
                break;
            }

            for (TDuration d : dList) {
                if (ltuplet) {
                    // take care not to recreate tuplet we just deleted
                    Rest* r = setRest(tick, track, d.fraction(), false, 0, false);
                    tick += r->actualTicks();
                } else {
                    tick += addClone(cr, tick, d)->actualTicks();
                }
            }
            break;
        }
        accumulated += td;
        sd          -= td;
        if (sd.isZero()) {
            break;
        }
    }
//      Fraction ticks = measure->tick() + measure->ticks() - segment->tick();
//      Fraction td = Fraction::fromTicks(ticks);
// NEEDS REVIEW !!
// once the statement below is removed, these two lines do nothing
//      if (td > sd)
//            td = sd;
// ???  accumulated should already contain the total value of the created gap: line 749, 811 or 838
//      this line creates a double-sized gap if the needed gap crosses a measure boundary
//      by adding again the duration already added in line 838
//      accumulated += td;

    const Fraction t1 = firstSegmentEnd;
    const Fraction t2 = firstSegment->tick() + accumulated;
    if (t1 < t2) {
        Segment* s1 = tick2rightSegment(t1);
        Segment* s2 = tick2rightSegment(t2);
        typedef SelectionFilterType Sel;
        // chord symbols can exist without chord/rest so they should not be removed
        constexpr Sel filter = static_cast<Sel>(int(Sel::ALL) & ~int(Sel::CHORD_SYMBOL));
        deleteAnnotationsFromRange(s1, s2, track, track + 1, filter);
        deleteSpannersFromRange(t1, t2, track, track + 1, filter);
    }

    return accumulated;
}

//---------------------------------------------------------
//   makeGap1
//    make time gap for each voice
//    starting at tick+voiceOffset[voice] by removing/shortening
//    chord/rest
//    - cr is top level (not part of a tuplet)
//    - do not stop at measure end
//---------------------------------------------------------

bool Score::makeGap1(const Fraction& baseTick, staff_idx_t staffIdx, const Fraction& len, int voiceOffset[VOICES])
{
    Segment* seg = tick2segment(baseTick, true, SegmentType::ChordRest);
    if (!seg) {
        LOGD("no segment to paste at tick %d", baseTick.ticks());
        return false;
    }
    track_idx_t strack = staffIdx * VOICES;
    for (track_idx_t track = strack; track < strack + VOICES; track++) {
        if (voiceOffset[track - strack] == -1) {
            continue;
        }
        Fraction tick = baseTick + Fraction::fromTicks(voiceOffset[track - strack]);
        Measure* m   = tick2measure(tick);
        if ((track % VOICES) && !m->hasVoices(staffIdx)) {
            continue;
        }

        Fraction newLen = len - Fraction::fromTicks(voiceOffset[track - strack]);
        assert(newLen.numerator() != 0);

        if (newLen > Fraction(0, 1)) {
            const Fraction endTick = tick + newLen;
            typedef SelectionFilterType Sel;
            // chord symbols can exist without chord/rest so they should not be removed
            constexpr Sel filter = static_cast<Sel>(int(Sel::ALL) & ~int(Sel::CHORD_SYMBOL));
            deleteAnnotationsFromRange(tick2rightSegment(tick), tick2rightSegment(endTick), track, track + 1, filter);
            deleteSpannersFromRange(tick, endTick, track, track + 1, filter);
        }

        seg = m->undoGetSegment(SegmentType::ChordRest, tick);
        bool result = makeGapVoice(seg, track, newLen, tick);
        if (track == strack && !result) {   // makeGap failed for first voice
            return false;
        }
    }
    return true;
}

bool Score::makeGapVoice(Segment* seg, track_idx_t track, Fraction len, const Fraction& tick)
{
    ChordRest* cr = 0;
    cr = toChordRest(seg->element(track));
    if (!cr) {
        // check if we are in the middle of a chord/rest
        Segment* seg1 = seg->prev(SegmentType::ChordRest);
        for (;;) {
            if (seg1 == 0) {
                if (!(track % VOICES)) {
                    LOGD("no segment before tick %d", tick.ticks());
                }
                // this happens only for voices other than voice 1
                expandVoice(seg, track);
                return makeGapVoice(seg, track, len, tick);
            }
            if (seg1->element(track)) {
                break;
            }
            seg1 = seg1->prev(SegmentType::ChordRest);
        }
        ChordRest* cr1 = toChordRest(seg1->element(track));
        Fraction srcF = cr1->ticks();
        Fraction dstF = tick - cr1->tick();
        std::vector<TDuration> dList = toDurationList(dstF, true);
        size_t n = dList.size();
        undoChangeChordRestLen(cr1, TDuration(dList[0]));
        if (n > 1) {
            Fraction crtick = cr1->tick() + cr1->actualTicks();
            Measure* measure = tick2measure(crtick);
            if (cr1->type() == ElementType::CHORD) {
                // split Chord
                Chord* c = toChord(cr1);
                for (size_t i = 1; i < n; ++i) {
                    TDuration d = dList[i];
                    Chord* c2 = addChord(crtick, d, c, true, c->tuplet());
                    c = c2;
                    seg1 = c->segment();
                    crtick += c->actualTicks();
                }
            } else {
                // split Rest
                Rest* r       = toRest(cr1);
                for (size_t i = 1; i < n; ++i) {
                    TDuration d = dList[i];
                    Rest* r2      = toRest(r->clone());
                    r2->setTicks(d.fraction());
                    r2->setDurationType(d);
                    undoAddCR(r2, measure, crtick);
                    seg1 = r2->segment();
                    crtick += r2->actualTicks();
                }
            }
        }
        setRest(tick, track, srcF - dstF, true, 0);
        for (;;) {
            seg1 = seg1->next1(SegmentType::ChordRest);
            if (seg1 == 0) {
                LOGD("no segment");
                return false;
            }
            if (seg1->element(track)) {
                cr = toChordRest(seg1->element(track));
                break;
            }
        }
    }

    for (;;) {
        if (!cr) {
            LOGD("cannot make gap");
            return false;
        }
        Fraction l = makeGap(cr->segment(), cr->track(), len, 0);
        if (l.isZero()) {
            LOGD("returns zero gap");
            return false;
        }
        len -= l;
        if (len.isZero()) {
            break;
        }
        // go to next cr
        Measure* m = cr->measure()->nextMeasure();
        if (!m) {
            LOGD("EOS reached");
            InsertMeasureOptions options;
            options.createEmptyMeasures = false;
            insertMeasure(ElementType::MEASURE, nullptr, options);
            m = cr->measure()->nextMeasure();
            if (!m) {
                LOGD("===EOS reached");
                return true;
            }
        }
        // first segment in measure was removed, have to recreate it
        Segment* s = m->undoGetSegment(SegmentType::ChordRest, m->tick());
        track_idx_t t = cr->track();
        cr = toChordRest(s->element(t));
        if (!cr) {
            addRest(s, t, TDuration(DurationType::V_MEASURE), 0);
            cr = toChordRest(s->element(t));
        }
    }
    return true;
}

//---------------------------------------------------------
//   splitGapToMeasureBoundaries
//    cr  - start of gap
//    gap - gap len
//---------------------------------------------------------

std::list<Fraction> Score::splitGapToMeasureBoundaries(ChordRest* cr, Fraction gap)
{
    std::list<Fraction> flist;

    Tuplet* tuplet = cr->tuplet();
    if (tuplet) {
        Fraction rest = tuplet->elementsDuration();
        for (DurationElement* de : tuplet->elements()) {
            if (de == cr) {
                break;
            }
            rest -= de->ticks();
        }
        if (rest < gap) {
            LOGD("does not fit in tuplet");
        } else {
            flist.push_back(gap);
        }
        return flist;
    }

    Segment* s = cr->segment();
    while (gap > Fraction(0, 1)) {
        Measure* m    = s->measure();
        Fraction timeStretch = cr->staff()->timeStretch(s->tick());
        Fraction rest = (m->ticks() - s->rtick()) * timeStretch;
        if (rest >= gap) {
            flist.push_back(gap);
            return flist;
        }
        flist.push_back(rest);
        gap -= rest;
        m = m->nextMeasure();
        if (m == 0) {
            return flist;
        }
        s = m->first(SegmentType::ChordRest);
    }
    return flist;
}

//---------------------------------------------------------
//   changeCRlen
//---------------------------------------------------------

void Score::changeCRlen(ChordRest* cr, const TDuration& d)
{
    Fraction dstF;
    if (d.type() == DurationType::V_MEASURE) {
        dstF = cr->measure()->stretchedLen(cr->staff());
    } else {
        dstF = d.fraction();
    }
    changeCRlen(cr, dstF);
}

void Score::changeCRlen(ChordRest* cr, const Fraction& dstF, bool fillWithRest)
{
    if (cr->isMeasureRepeat()) {
        // it is not clear what should this
        // operation mean for measure repeats.
        return;
    }
    Fraction srcF(cr->ticks());
    if (srcF == dstF) {
        if (cr->isFullMeasureRest()) {
            undoChangeChordRestLen(cr, dstF);
        }
        return;
    }

    //keep selected element if any
    EngravingItem* selElement = selection().isSingle() ? getSelectedElement() : 0;

    track_idx_t track = cr->track();
    Tuplet* tuplet = cr->tuplet();

    if (srcF > dstF) {
        //
        // make shorter and fill with rest
        //
        deselectAll();
        if (cr->isChord()) {
            //
            // remove ties and tremolo between 2 notes
            //
            Chord* c = toChord(cr);
            if (c->tremolo()) {
                Tremolo* tremolo = c->tremolo();
                if (tremolo->twoNotes()) {
                    undoRemoveElement(tremolo);
                }
            }
            for (Note* n : c->notes()) {
                if (n->tieFor()) {
                    undoRemoveElement(n->tieFor());
                }
            }
        }
        Fraction timeStretch = cr->staff()->timeStretch(cr->tick());
        std::vector<TDuration> dList = toDurationList(dstF, true);
        undoChangeChordRestLen(cr, dList[0]);
        Fraction tick2 = cr->tick();
        for (unsigned i = 1; i < dList.size(); ++i) {
            tick2 += actualTicks(dList[i - 1].ticks(), tuplet, timeStretch);
            TDuration d = dList[i];
            setRest(tick2, track, d.fraction(), (d.dots() > 0), tuplet);
        }
        if (fillWithRest) {
            setRest(cr->tick() + cr->actualTicks(), track, srcF - dstF, false, tuplet);
        }

        if (selElement) {
            select(selElement, SelectType::SINGLE, 0);
        }
        return;
    }

    //
    // make longer
    //
    // split required len into Measures
    std::list<Fraction> flist = splitGapToMeasureBoundaries(cr, dstF);
    if (flist.empty()) {
        return;
    }

    deselectAll();
    EngravingItem* elementToSelect = nullptr;

    Fraction tick  = cr->tick();
    Fraction f     = dstF;
    ChordRest* cr1 = cr;
    Chord* oc      = 0;

    bool first = true;
    for (const Fraction& f2 : flist) {
        f  -= f2;
        makeGap(cr1->segment(), cr1->track(), f2, tuplet, first);

        if (cr->isRest()) {
            Fraction timeStretch = cr1->staff()->timeStretch(cr1->tick());
            Rest* r = toRest(cr);
            if (first) {
                std::vector<TDuration> dList = toDurationList(f2, true);
                undoChangeChordRestLen(cr, dList[0]);
                Fraction tick2 = cr->tick();
                for (unsigned i = 1; i < dList.size(); ++i) {
                    tick2 += actualTicks(dList[i - 1].ticks(), tuplet, timeStretch);
                    TDuration d = dList[i];
                    setRest(tick2, track, d.fraction(), (d.dots() > 0), tuplet);
                }
            } else {
                r = setRest(tick, track, f2, false, tuplet);
            }
            if (first) {
                elementToSelect = r;
                first = false;
            }
            tick += actualTicks(f2, tuplet, timeStretch);
        } else {
            std::vector<TDuration> dList = toDurationList(f2, true);
            Measure* measure             = tick2measure(tick);
            Fraction etick                    = measure->tick();

            if (((tick - etick).ticks() % dList[0].ticks().ticks()) == 0) {
                for (TDuration du : dList) {
                    Chord* cc = nullptr;
                    if (oc) {
                        cc = oc;
                        oc = addChord(tick, du, cc, true, tuplet);
                    } else {
                        cc = toChord(cr);
                        undoChangeChordRestLen(cr, du);
                        oc = cc;
                    }
                    if (oc && first) {
                        elementToSelect = selElement ? selElement : oc;
                        first = false;
                    }
                    if (oc) {
                        tick += oc->actualTicks();
                    }
                }
            } else {
                for (size_t i = dList.size(); i > 0; --i) {         // loop probably needs to be in this reverse order
                    Chord* cc;
                    if (oc) {
                        cc = oc;
                        oc = addChord(tick, dList[i - 1], cc, true, tuplet);
                    } else {
                        cc = toChord(cr);
                        undoChangeChordRestLen(cr, dList[i - 1]);
                        oc = cc;
                    }
                    if (first) {
                        elementToSelect = selElement;
                        first = false;
                    }
                    tick += oc->actualTicks();
                }
            }
        }
        Measure* m  = cr1->measure();
        Measure* m1 = m->nextMeasure();
        if (m1 == 0) {
            break;
        }
        Segment* s = m1->first(SegmentType::ChordRest);
        expandVoice(s, track);
        cr1 = toChordRest(s->element(track));
    }
    connectTies();

    if (elementToSelect) {
        if (containsElement(elementToSelect)) {
            select(elementToSelect, SelectType::SINGLE, 0);
        }
    }
}

//---------------------------------------------------------
//   upDownChromatic
//---------------------------------------------------------

static void upDownChromatic(bool up, int pitch, Note* n, Key key, int tpc1, int tpc2, int& newPitch, int& newTpc1, int& newTpc2)
{
    if (up && pitch < 127) {
        newPitch = pitch + 1;
        if (n->concertPitch()) {
            if (tpc1 > Tpc::TPC_A + int(key)) {
                newTpc1 = tpc1 - 5;           // up semitone diatonic
            } else {
                newTpc1 = tpc1 + 7;           // up semitone chromatic
            }
            newTpc2 = n->transposeTpc(newTpc1);
        } else {
            if (tpc2 > Tpc::TPC_A + int(key)) {
                newTpc2 = tpc2 - 5;           // up semitone diatonic
            } else {
                newTpc2 = tpc2 + 7;           // up semitone chromatic
            }
            newTpc1 = n->transposeTpc(newTpc2);
        }
    } else if (!up && pitch > 0) {
        newPitch = pitch - 1;
        if (n->concertPitch()) {
            if (tpc1 > Tpc::TPC_C + int(key)) {
                newTpc1 = tpc1 - 7;           // down semitone chromatic
            } else {
                newTpc1 = tpc1 + 5;           // down semitone diatonic
            }
            newTpc2 = n->transposeTpc(newTpc1);
        } else {
            if (tpc2 > Tpc::TPC_C + int(key)) {
                newTpc2 = tpc2 - 7;           // down semitone chromatic
            } else {
                newTpc2 = tpc2 + 5;           // down semitone diatonic
            }
            newTpc1 = n->transposeTpc(newTpc2);
        }
    }
}

//---------------------------------------------------------
//   setTpc
//---------------------------------------------------------

static void setTpc(Note* oNote, int tpc, int& newTpc1, int& newTpc2)
{
    if (oNote->concertPitch()) {
        newTpc1 = tpc;
        newTpc2 = oNote->transposeTpc(tpc);
    } else {
        newTpc2 = tpc;
        newTpc1 = oNote->transposeTpc(tpc);
    }
}

//---------------------------------------------------------
//   upDown
///   Increment/decrement pitch of note by one or by an octave.
//---------------------------------------------------------

void Score::upDown(bool up, UpDownMode mode)
{
    std::list<Note*> el = selection().uniqueNotes();

    for (Note* oNote : el) {
        Fraction tick     = oNote->chord()->tick();
        Staff* staff = oNote->staff();
        Part* part   = staff->part();
        Key key      = staff->key(tick);
        int tpc1     = oNote->tpc1();
        int tpc2     = oNote->tpc2();
        int pitch    = oNote->pitch();
        int newTpc1  = tpc1;          // default to unchanged
        int newTpc2  = tpc2;          // default to unchanged
        int newPitch = pitch;         // default to unchanged
        int string   = oNote->string();
        int fret     = oNote->fret();

        StaffGroup staffGroup = staff->staffType(oNote->chord()->tick())->group();
        // if not tab, check for instrument instead of staffType (for pitched to unpitched instrument changes)
        if (staffGroup != StaffGroup::TAB) {
            staffGroup = staff->part()->instrument(oNote->tick())->useDrumset() ? StaffGroup::PERCUSSION : StaffGroup::STANDARD;
        }

        switch (staffGroup) {
        case StaffGroup::PERCUSSION:
        {
            const Drumset* ds = part->instrument(tick)->drumset();
            if (ds) {
                newPitch = up ? ds->nextPitch(pitch) : ds->prevPitch(pitch);
                newTpc1 = pitch2tpc(newPitch, Key::C, Prefer::NEAREST);
                newTpc2 = newTpc1;
            }
        }
        break;
        case StaffGroup::TAB:
        {
            const StringData* stringData = part->instrument(tick)->stringData();
            switch (mode) {
            case UpDownMode::OCTAVE:                            // move same note to next string, if possible
            {
                const StaffType* stt = staff->staffType(tick);
                string = stt->physStringToVisual(string);
                string += (up ? -1 : 1);
                if (string < 0 || string >= static_cast<int>(stringData->strings())) {
                    return;                                 // no next string to move to
                }
                string = stt->visualStringToPhys(string);
                fret = stringData->fret(pitch, string, staff);
                if (fret == -1) {                            // can't have that note on that string
                    return;
                }
                // newPitch and newTpc remain unchanged
            }
            break;

            case UpDownMode::DIATONIC:                          // increase / decrease the pitch,
                // letting the algorithm to choose fret & string
                upDownChromatic(up, pitch, oNote, key, tpc1, tpc2, newPitch, newTpc1, newTpc2);
                break;

            case UpDownMode::CHROMATIC:                         // increase / decrease the fret
            {                                               // without changing the string
                // compute new fret
                if (!stringData->frets()) {
                    LOGD("upDown tab chromatic: no frets?");
                    return;
                }
                fret += (up ? 1 : -1);
                if (fret < 0 || fret > stringData->frets()) {
                    LOGD("upDown tab in-string: out of fret range");
                    return;
                }
                // update pitch and tpc's and check it matches stringData
                upDownChromatic(up, pitch, oNote, key, tpc1, tpc2, newPitch, newTpc1, newTpc2);
                if (newPitch != stringData->getPitch(string, fret, staff)) {
                    // oh-oh: something went very wrong!
                    LOGD("upDown tab in-string: pitch mismatch");
                    return;
                }
                // store the fretting change before undoChangePitch() chooses
                // a fretting of its own liking!
                oNote->undoChangeProperty(Pid::FRET, fret);
            }
            break;
            }
        }
        break;
        case StaffGroup::STANDARD:
            switch (mode) {
            case UpDownMode::OCTAVE:
                if (up) {
                    if (pitch < 116) {
                        newPitch = pitch + 12;
                    }
                } else {
                    if (pitch > 11) {
                        newPitch = pitch - 12;
                    }
                }
                // newTpc remains unchanged
                break;

            case UpDownMode::CHROMATIC:
                upDownChromatic(up, pitch, oNote, key, tpc1, tpc2, newPitch, newTpc1, newTpc2);
                break;

            case UpDownMode::DIATONIC:
            {
                int tpc = oNote->tpc();
                if (up) {
                    if (tpc > Tpc::TPC_A + int(key)) {
                        if (pitch < 127) {
                            newPitch = pitch + 1;
                            setTpc(oNote, tpc - 5, newTpc1, newTpc2);
                        }
                    } else {
                        if (pitch < 126) {
                            newPitch = pitch + 2;
                            setTpc(oNote, tpc + 2, newTpc1, newTpc2);
                        }
                    }
                } else {
                    if (tpc > Tpc::TPC_C + int(key)) {
                        if (pitch > 1) {
                            newPitch = pitch - 2;
                            setTpc(oNote, tpc - 2, newTpc1, newTpc2);
                        }
                    } else {
                        if (pitch > 0) {
                            newPitch = pitch - 1;
                            setTpc(oNote, tpc + 5, newTpc1, newTpc2);
                        }
                    }
                }
            }
            break;
            }
            break;
        }

        if ((oNote->pitch() != newPitch) || (oNote->tpc1() != newTpc1) || oNote->tpc2() != newTpc2) {
            // remove accidental if present to make sure
            // user added accidentals are removed here
            // unless it's an octave change
            // in this case courtesy accidentals are preserved
            // because they're now harder to be re-entered due to the revised note-input workflow
            if (mode != UpDownMode::OCTAVE) {
                auto l = oNote->linkList();
                for (EngravingObject* e : l) {
                    Note* ln = toNote(e);
                    if (ln->accidental()) {
                        undo(new RemoveElement(ln->accidental()));
                    }
                }
            }
            undoChangePitch(oNote, newPitch, newTpc1, newTpc2);
        }
        // store fret change only if undoChangePitch has not been called,
        // as undoChangePitch() already manages fret changes, if necessary
        else if (staff->staffType(tick)->group() == StaffGroup::TAB) {
            bool refret = false;
            if (oNote->string() != string) {
                oNote->undoChangeProperty(Pid::STRING, string);
                refret = true;
            }
            if (oNote->fret() != fret) {
                oNote->undoChangeProperty(Pid::FRET, fret);
                refret = true;
            }
            if (refret) {
                const StringData* stringData = part->instrument(tick)->stringData();
                stringData->fretChords(oNote->chord());
            }
        }

        // play new note with velocity 80 for 0.3 sec:
        setPlayNote(true);
    }
    setSelectionChanged(true);
}

//---------------------------------------------------------
//   upDownDelta
///   Add the delta to the pitch of note.
//---------------------------------------------------------

void Score::upDownDelta(int pitchDelta)
{
    while (pitchDelta > 0) {
        upDown(true, UpDownMode::CHROMATIC);
        pitchDelta--;
    }

    while (pitchDelta < 0) {
        upDown(false, UpDownMode::CHROMATIC);
        pitchDelta++;
    }
}

//---------------------------------------------------------
//   toggleArticulation
///   Toggle attribute \a attr for all selected notes/rests.
///
///   Called from padToggle() to add note prefix/accent.
//---------------------------------------------------------

void Score::toggleArticulation(SymId attr)
{
    std::set<Chord*> set;
    int numAdded = 0;
    int numRemoved = 0;
    for (EngravingItem* el : selection().elements()) {
        if (el->isNote() || el->isChord()) {
            Chord* cr = 0;
            // apply articulation on a given chord only once
            if (el->isNote()) {
                cr = toNote(el)->chord();
                if (mu::contains(set, cr)) {
                    continue;
                }
            }
            Articulation* na = Factory::createArticulation(this->dummy()->chord());
            na->setSymId(attr);
            if (toggleArticulation(el, na)) {
                ++numAdded;
            } else {
                delete na;
                ++numRemoved;
            }

            if (cr) {
                set.insert(cr);
            }
        }
    }
}

//---------------------------------------------------------
//   toggleAccidental
//---------------------------------------------------------

void Score::toggleAccidental(AccidentalType at, const EditData& ed)
{
    if (_is.accidentalType() == at) {
        at = AccidentalType::NONE;
    }
    if (noteEntryMode()) {
        _is.setAccidentalType(at);
        _is.setRest(false);
    } else {
        if (selection().isNone()) {
            ed.view()->startNoteEntryMode();
            _is.setAccidentalType(at);
            _is.setDuration(DurationType::V_QUARTER);
            _is.setRest(false);
        } else {
            changeAccidental(at);
        }
    }
}

//---------------------------------------------------------
//   changeAccidental
///   Change accidental to subtype \a idx for all selected
///   notes.
//---------------------------------------------------------

void Score::changeAccidental(AccidentalType idx)
{
    for (Note* note : selection().noteList()) {
        changeAccidental(note, idx);
    }
}

//---------------------------------------------------------
//   changeAccidental2
//---------------------------------------------------------

static void changeAccidental2(Note* n, int pitch, int tpc)
{
    Score* score  = n->score();
    Chord* chord  = n->chord();
    Staff* st     = chord->staff();
    int fret      = n->fret();
    int string    = n->string();

    if (st->isTabStaff(chord->tick())) {
        if (pitch != n->pitch()) {
            //
            // as pitch has changed, calculate new
            // string & fret
            //
            const StringData* stringData = n->part()->instrument(n->tick())->stringData();
            if (stringData) {
                stringData->convertPitch(pitch, st, &string, &fret);
            }
        }
    }
    int tpc1;
    int tpc2 = n->transposeTpc(tpc);
    if (score->styleB(Sid::concertPitch)) {
        tpc1 = tpc;
    } else {
        tpc1 = tpc2;
        tpc2 = tpc;
    }

    if (!st->isTabStaff(chord->tick())) {
        //
        // handle ties
        //
        if (n->tieBack()) {
            if (pitch != n->pitch()) {
                score->undoRemoveElement(n->tieBack());
                if (n->tieFor()) {
                    score->undoRemoveElement(n->tieFor());
                }
            }
        } else {
            Note* nn = n;
            while (nn->tieFor()) {
                nn = nn->tieFor()->endNote();
                score->undo(new ChangePitch(nn, pitch, tpc1, tpc2));
            }
        }
    }
    score->undoChangePitch(n, pitch, tpc1, tpc2);
}

//---------------------------------------------------------
//   changeAccidental
///   Change accidental to subtype \accidental for
///   note \a note.
//---------------------------------------------------------

void Score::changeAccidental(Note* note, AccidentalType accidental)
{
    Chord* chord = note->chord();
    if (!chord) {
        return;
    }
    Segment* segment = chord->segment();
    if (!segment) {
        return;
    }
    Measure* measure = segment->measure();
    if (!measure) {
        return;
    }
    Fraction tick = segment->tick();
    Staff* estaff = staff(chord->staffIdx() + chord->staffMove());
    if (!estaff) {
        return;
    }
    ClefType clef = estaff->clef(tick);
    int step      = ClefInfo::pitchOffset(clef) - note->line();
    while (step < 0) {
        step += 7;
    }
    step %= 7;
    //
    // accidental change may result in pitch change
    //
    AccidentalVal acc2 = measure->findAccidental(note);
    AccidentalVal acc = (accidental == AccidentalType::NONE) ? acc2 : Accidental::subtype2value(accidental);

    int pitch = line2pitch(note->line(), clef, Key::C) + int(acc);
    if (!note->concertPitch()) {
        pitch += note->transposition();
    }

    int tpc = step2tpc(step, acc);

    bool forceRemove = false;
    bool forceAdd = false;

    // delete accidental
    // both for this note and for any linked notes
    if (accidental == AccidentalType::NONE) {
        forceRemove = true;
    }
    // precautionary or microtonal accidental
    // either way, we display it unconditionally
    // both for this note and for any linked notes
    else if (acc == acc2 || (pitch == note->pitch() && !Accidental::isMicrotonal(note->accidentalType()))
             || Accidental::isMicrotonal(accidental)) {
        forceAdd = true;
    }

    for (EngravingObject* se : note->linkList()) {
        Note* ln = toNote(se);
        if (ln->concertPitch() != note->concertPitch()) {
            continue;
        }
        Score* lns    = ln->score();
        Accidental* a = ln->accidental();
        if (forceRemove) {
            if (a) {
                lns->undoRemoveElement(a);
            }
            if (ln->tieBack()) {
                continue;
            }
        } else if (forceAdd) {
            if (a) {
                undoRemoveElement(a);
            }
            Accidental* a1 = Factory::createAccidental(ln);
            a1->setParent(ln);
            a1->setAccidentalType(accidental);
            a1->setRole(AccidentalRole::USER);
            lns->undoAddElement(a1);
        } else if (a && Accidental::isMicrotonal(a->accidentalType())) {
            lns->undoRemoveElement(a);
        }
        changeAccidental2(ln, pitch, tpc);
    }
    setPlayNote(true);
    setSelectionChanged(true);
}

//---------------------------------------------------------
//   toggleArticulation
//---------------------------------------------------------

bool Score::toggleArticulation(EngravingItem* el, Articulation* a)
{
    Chord* c;
    if (el->isNote()) {
        c = toNote(el)->chord();
    } else if (el->isChord()) {
        c = toChord(el);
    } else {
        return false;
    }
    Articulation* oa = c->hasArticulation(a);
    if (oa) {
        undoRemoveElement(oa);
        return false;
    }
    a->setParent(c);
    a->setTrack(c->track());   // make sure it propagates between score and parts
    undoAddElement(a);
    return true;
}

//---------------------------------------------------------
//   resetUserStretch
//---------------------------------------------------------

void Score::resetUserStretch()
{
    Measure* m1 = nullptr;
    Measure* m2 = nullptr;
    // retrieve span of selection
    Segment* s1 = _selection.startSegment();
    Segment* s2 = _selection.endSegment();
    // if either segment is not returned by the selection
    // (for instance, no selection) fall back to first/last measure
    if (!s1) {
        m1 = firstMeasureMM();
    } else {
        m1 = s1->measure();
    }
    if (!s2) {
        m2 = lastMeasureMM();
    } else {
        m2 = s2->measure();
    }
    if (!m1 || !m2) {               // should not happen!
        return;
    }

    for (Measure* m = m1; m; m = m->nextMeasureMM()) {
        m->undoChangeProperty(Pid::USER_STRETCH, 1.0);
        if (m == m2) {
            break;
        }
    }
}

//---------------------------------------------------------
//   moveUp
//---------------------------------------------------------

void Score::moveUp(ChordRest* cr)
{
    Staff* staff  = cr->staff();
    Part* part    = staff->part();
    staff_idx_t rstaff    = staff->rstaff();
    int staffMove = cr->staffMove();

    if ((staffMove == -1) || (static_cast<int>(rstaff) + staffMove <= 0)) {
        return;
    }

    const std::vector<Staff*>& staves = part->staves();
    // we know that staffMove+rstaff-1 index exists due to the previous condition.
    if (staff->staffType(cr->tick())->group() != StaffGroup::STANDARD
        || staves.at(rstaff + staffMove - 1)->staffType(cr->tick())->group() != StaffGroup::STANDARD) {
        LOGD("User attempted to move a note from/to a staff which does not use standard notation - ignoring.");
    } else {
        // move the chord up a staff
        undo(new ChangeChordStaffMove(cr, staffMove - 1));
    }
}

//---------------------------------------------------------
//   moveDown
//---------------------------------------------------------

void Score::moveDown(ChordRest* cr)
{
    Staff* staff  = cr->staff();
    Part* part    = staff->part();
    staff_idx_t rstaff = staff->rstaff();
    int staffMove = cr->staffMove();
    // calculate the number of staves available so that we know whether there is another staff to move down to
    size_t rstaves = part->nstaves();

    if ((staffMove == 1) || (rstaff + staffMove >= rstaves - 1)) {
        LOGD("moveDown staffMove==%d  rstaff %zu rstaves %zu", staffMove, rstaff, rstaves);
        return;
    }

    const std::vector<Staff*>& staves = part->staves();
    // we know that staffMove+rstaff+1 index exists due to the previous condition.
    if (staff->staffType(cr->tick())->group() != StaffGroup::STANDARD
        || staves.at(staffMove + rstaff + 1)->staffType(cr->tick())->group() != StaffGroup::STANDARD) {
        LOGD("User attempted to move a note from/to a staff which does not use standard notation - ignoring.");
    } else {
        // move the chord down a staff
        undo(new ChangeChordStaffMove(cr, staffMove + 1));
    }
}

//---------------------------------------------------------
//   cmdAddStretch
//---------------------------------------------------------

void Score::cmdAddStretch(double val)
{
    if (!selection().isRange()) {
        return;
    }
    Fraction startTick = selection().tickStart();
    Fraction endTick   = selection().tickEnd();
    for (Measure* m = firstMeasureMM(); m; m = m->nextMeasureMM()) {
        if (m->tick() < startTick) {
            continue;
        }
        if (m->tick() >= endTick) {
            break;
        }
        double stretch = m->userStretch();
        stretch += val;
        if (stretch < 0) {
            stretch = 0;
        }
        m->undoChangeProperty(Pid::USER_STRETCH, stretch);
    }
}

//---------------------------------------------------------
//   cmdResetBeamMode
//---------------------------------------------------------

void Score::cmdResetBeamMode()
{
    bool noSelection = selection().isNone();
    if (noSelection) {
        cmdSelectAll();
    } else if (!selection().isRange()) {
        LOGD("no system or staff selected");
        return;
    }

    Fraction endTick = selection().tickEnd();

    for (Segment* seg = selection().firstChordRestSegment(); seg && seg->tick() < endTick; seg = seg->next1(SegmentType::ChordRest)) {
        for (track_idx_t track = selection().staffStart() * VOICES; track < selection().staffEnd() * VOICES; ++track) {
            ChordRest* cr = toChordRest(seg->element(track));
            if (!cr) {
                continue;
            }
            if (cr->type() == ElementType::CHORD) {
                if (cr->beamMode() != BeamMode::AUTO) {
                    cr->undoChangeProperty(Pid::BEAM_MODE, BeamMode::AUTO);
                }
            } else if (cr->type() == ElementType::REST) {
                if (cr->beamMode() != BeamMode::NONE) {
                    cr->undoChangeProperty(Pid::BEAM_MODE, BeamMode::NONE);
                }
            }
        }
    }
    if (noSelection) {
        deselectAll();
    }
}

void Score::cmdResetTextStyleOverrides()
{
    static const std::vector<Pid> propertiesToReset {
        Pid::FONT_FACE,
        Pid::FONT_SIZE,
        Pid::FONT_STYLE,
        Pid::SIZE_SPATIUM_DEPENDENT,
        Pid::FRAME_TYPE,
        Pid::TEXT_LINE_SPACING,
        Pid::FRAME_FG_COLOR,
        Pid::FRAME_BG_COLOR,
        Pid::FRAME_WIDTH,
        Pid::FRAME_PADDING,
        Pid::FRAME_ROUND,
        Pid::ALIGN
    };

    for (Page* page : pages()) {
        auto elements = page->elements();

        for (EngravingItem* element : elements) {
            if (!element || !element->isTextBase()) {
                continue;
            }

            for (Pid propertyId : propertiesToReset) {
                element->resetProperty(propertyId);
            }
        }
    }
}

//---------------------------------------------------------
//   cmdResetNoteAndRestGroupings
//---------------------------------------------------------

void Score::cmdResetNoteAndRestGroupings()
{
    bool noSelection = selection().isNone();
    if (noSelection) {
        cmdSelectAll();
    } else if (!selection().isRange()) {
        LOGD("no system or staff selected");
        return;
    }

    // save selection values because selection changes during grouping
    Fraction sTick = selection().tickStart();
    Fraction eTick = selection().tickEnd();
    staff_idx_t sStaff = selection().staffStart();
    staff_idx_t eStaff = selection().staffEnd();

    startCmd();
    for (staff_idx_t staff = sStaff; staff < eStaff; staff++) {
        track_idx_t sTrack = staff * VOICES;
        track_idx_t eTrack = sTrack + VOICES;
        for (track_idx_t track = sTrack; track < eTrack; track++) {
            if (selectionFilter().canSelectVoice(track)) {
                regroupNotesAndRests(sTick, eTick, track);
            }
        }
    }
    endCmd();
    if (noSelection) {
        deselectAll();
    }
}

//---------------------------------------------------------
//   resetElementShapePosition
//    For use with Score::scanElements.
//    Reset positions and autoplacement for the given
//    element.
//---------------------------------------------------------

static void resetElementPosition(void*, EngravingItem* e)
{
    if (e->generated()) {
        return;
    }
    e->undoResetProperty(Pid::AUTOPLACE);
    e->undoResetProperty(Pid::OFFSET);
    e->setOffsetChanged(false);
    if (e->isSpanner()) {
        e->undoResetProperty(Pid::OFFSET2);
    }
}

//---------------------------------------------------------
//   cmdResetAllPositions
//---------------------------------------------------------

void Score::cmdResetAllPositions(bool undoable)
{
    if (undoable) {
        startCmd();
    }
    resetAutoplace();
    if (undoable) {
        endCmd();
    }
}

//---------------------------------------------------------
//   resetAutoplace
//---------------------------------------------------------

void Score::resetAutoplace()
{
    TRACEFUNC;

    scanElements(nullptr, resetElementPosition);
}

//---------------------------------------------------------
//   resetDefaults
//    Resets all custom positioning stuff (except for direction). Used in score migration.
//---------------------------------------------------------

void Score::resetDefaults()
{
    TRACEFUNC;

    // layout stretch for pre-4.0 scores will be reset
    resetUserStretch();

    // all system objects should be cleared as of now, since pre-4.0 scores don't have a <SystemObjects> tag
    clearSystemObjectStaves();

    for (System* sys : systems()) {
        for (MeasureBase* mb : sys->measures()) {
            if (!mb->isMeasure()) {
                continue;
            }
            Measure* m = toMeasure(mb);
            for (Segment* seg = m->first(); seg; seg = seg->next()) {
                if (seg->isChordRestType()) {
                    for (EngravingItem* e : seg->elist()) {
                        if (!e || !e->isChord()) {
                            continue;
                        }
                        Chord* c = toChord(e);
                        if (c->stem()) {
                            c->stem()->undoChangeProperty(Pid::USER_LEN, Millimetre(0.0));
                        }
                        if (c->tremolo()) {
                            c->tremolo()->roffset() = PointF();
                        }
                    }
                }
                for (EngravingItem* e : seg->annotations()) {
                    if (e->isDynamic()) {
                        Dynamic* d = toDynamic(e);
                        if (d->xmlText().contains(u"<sym>") && !d->xmlText().contains(u"<font")) {
                            d->setAlign(Align(AlignH::HCENTER, AlignV::BASELINE));
                        }
                        d->setSize(10.0);
                    }
                }
            }
        }
        for (SpannerSegment* spannerSegment : sys->spannerSegments()) {
            if (spannerSegment->isSlurTieSegment()) {
                bool retainDirection = true;
                SlurTieSegment* slurTieSegment = toSlurTieSegment(spannerSegment);
                if (slurTieSegment->slurTie()->isTie()) {
                    Tie* tie = toTie(slurTieSegment->slurTie());
                    if (tie->isInside()) {
                        retainDirection = false;
                    }
                }
                auto dir = slurTieSegment->slurTie()->slurDirection();
                bool autoplace = slurTieSegment->slurTie()->autoplace();
                slurTieSegment->reset();
                if (retainDirection) {
                    slurTieSegment->slurTie()->setSlurDirection(dir);
                }
                slurTieSegment->slurTie()->setAutoplace(autoplace);
            }
        }
    }
}

//---------------------------------------------------------
//   move
//    move current selection
//---------------------------------------------------------

EngravingItem* Score::move(const String& cmd)
{
    ChordRest* cr { nullptr };
    Box* box { nullptr };
    if (noteEntryMode()) {
        // if selection exists and is grace note, use it
        // otherwise use chord/rest at input position
        // also use it if we are moving to next chord
        // to catch up with the cursor and not move the selection by 2 positions
        cr = selection().cr();
        if (cr && (cr->isGrace() || cmd == u"next-chord" || cmd == u"prev-chord")) {
        } else {
            cr = inputState().cr();
        }
    } else if (selection().activeCR()) {
        cr = selection().activeCR();
    } else {
        cr = selection().lastChordRest();
    }

    // no chord/rest found? look for another type of element,
    // but commands [empty-trailing-measure] and [top-staff] don't
    // necessarily need an active selection for appropriate functioning
    if (!cr && cmd != u"empty-trailing-measure" && cmd != u"top-staff") {
        if (selection().elements().empty()) {
            return 0;
        }
        // retrieve last element of section list
        EngravingItem* el = selection().elements().back();
        EngravingItem* trg = nullptr;

        // get parent of element and process accordingly:
        // trg is the element to select on "next-chord" cmd
        // cr is the ChordRest to move from on other cmd's
        track_idx_t track = el->track();                // keep note of element track
        if (!el->isBox()) {
            el = el->parentItem();
        }
        // element with no parent (eg, a newly-added line) - no way to find context
        if (!el) {
            return 0;
        }
        switch (el->type()) {
        case ElementType::NOTE:                     // a note is a valid target
            trg = el;
            cr  = toNote(el)->chord();
            break;
        case ElementType::CHORD:                    // a chord or a rest are valid targets
        case ElementType::REST:
        case ElementType::MMREST:
            trg = el;
            cr  = toChordRest(trg);
            break;
        case ElementType::SEGMENT: {                // from segment go to top chordrest in segment
            Segment* seg  = toSegment(el);
            // if segment is not chord/rest or grace, move to next chord/rest or grace segment
            if (!seg->isChordRest()) {
                seg = seg->next1(SegmentType::ChordRest);
                if (!seg) {                 // if none found, return failure
                    return 0;
                }
            }
            // segment for sure contains chords/rests,
            size_t size = seg->elist().size();
            // if segment has a chord/rest in original element track, use it
            if (track < size && seg->element(track)) {
                trg  = seg->element(track);
                cr = toChordRest(trg);
                break;
            }
            // if not, get topmost chord/rest
            for (size_t i = 0; i < size; i++) {
                if (seg->element(i)) {
                    trg  = seg->element(i);
                    cr = toChordRest(trg);
                    break;
                }
            }
            break;
        }
        case ElementType::HBOX:           // fallthrough
        case ElementType::VBOX:           // fallthrough
        case ElementType::TBOX:
            box = toBox(el);
            break;
        default:                                // on anything else, return failure
            return 0;
        }

        // if something found and command is forward, the element found is the destination
        if (trg && cmd == u"next-chord") {
            // if chord, go to topmost note
            if (trg->type() == ElementType::CHORD) {
                trg = toChord(trg)->upNote();
            }
            setPlayNote(true);
            select(trg, SelectType::SINGLE, 0);
            return trg;
        }
        // if no chordrest and no box (frame) found, do nothing
        if (!cr && !box) {
            return 0;
        }
        // if some chordrest found, continue with default processing
    }

    EngravingItem* el = nullptr;
    Segment* ois = noteEntryMode() ? _is.segment() : nullptr;
    Measure* oim = ois ? ois->measure() : nullptr;

    if (cmd == u"next-chord" && cr) {
        // note input cursor
        if (noteEntryMode()) {
            _is.moveToNextInputPos();
        }

        // selection "cursor"
        // find next chordrest, which might be a grace note
        // this may override note input cursor
        el = nextChordRest(cr);
        while (el && el->isRest() && toRest(el)->isGap()) {
            el = nextChordRest(toChordRest(el));
        }
        if (el && noteEntryMode()) {
            // do not use if not in original or new measure (don't skip measures)
            Measure* m = toChordRest(el)->measure();
            Segment* nis = _is.segment();
            Measure* nim = nis ? nis->measure() : nullptr;
            if (m != oim && m != nim) {
                el = cr;
            }
            // do not use if new input segment is current cr
            // this means input cursor just caught up to current selection
            else if (cr && nis == cr->segment()) {
                el = cr;
            }
        } else if (!el) {
            el = cr;
        }
    } else if (cmd == u"prev-chord" && cr) {
        // note input cursor
        if (noteEntryMode() && _is.segment()) {
            Measure* m = _is.segment()->measure();
            Segment* s = _is.segment()->prev1(SegmentType::ChordRest);
            track_idx_t track = _is.track();
            for (; s; s = s->prev1(SegmentType::ChordRest)) {
                if (s->element(track) || (s->measure() != m && s->rtick().isZero())) {
                    if (s->element(track)) {
                        if (s->element(track)->isRest() && toRest(s->element(track))->isGap()) {
                            continue;
                        }
                    }
                    break;
                }
            }
            _is.moveInputPos(s);
        }

        // selection "cursor"
        // find previous chordrest, which might be a grace note
        // this may override note input cursor
        el = prevChordRest(cr);
        while (el && el->isRest() && toRest(el)->isGap()) {
            el = prevChordRest(toChordRest(el));
        }
        if (el && noteEntryMode()) {
            // do not use if not in original or new measure (don't skip measures)
            Measure* m = toChordRest(el)->measure();
            Segment* nis = _is.segment();
            Measure* nim = nis ? nis->measure() : nullptr;
            if (m != oim && m != nim) {
                el = cr;
            }
            // do not use if new input segment is current cr
            // this means input cursor just caught up to current selection
            else if (cr && nis == cr->segment()) {
                el = cr;
            }
        } else if (!el) {
            el = cr;
        }
    } else if (cmd == u"next-measure") {
        if (box && box->nextMeasure() && box->nextMeasure()->first()) {
            el = box->nextMeasure()->first()->nextChordRest(0, false);
        }
        if (cr) {
            el = nextMeasure(cr);
        }
        if (el && noteEntryMode()) {
            _is.moveInputPos(el);
        }
    } else if (cmd == u"prev-measure") {
        if (box && box->prevMeasure() && box->prevMeasure()->first()) {
            el = box->prevMeasure()->first()->nextChordRest(0, false);
        }
        if (cr) {
            el = prevMeasure(cr);
        }
        if (el && noteEntryMode()) {
            _is.moveInputPos(el);
        }
    } else if (cmd == u"next-system" && cr) {
        el = cmdNextPrevSystem(cr, true);
        if (noteEntryMode()) {
            _is.moveInputPos(el);
        }
    } else if (cmd == u"prev-system" && cr) {
        el = cmdNextPrevSystem(cr, false);
        if (noteEntryMode()) {
            _is.moveInputPos(el);
        }
    } else if (cmd == u"next-frame") {
        auto measureBase = cr ? cr->measure()->findMeasureBase() : box->findMeasureBase();
        el = measureBase ? cmdNextPrevFrame(measureBase, true) : nullptr;
    } else if (cmd == u"prev-frame") {
        auto measureBase = cr ? cr->measure()->findMeasureBase() : box->findMeasureBase();
        el = measureBase ? cmdNextPrevFrame(measureBase, false) : nullptr;
    } else if (cmd == u"next-section") {
        if (!(el = box)) {
            el = cr;
        }
        el = cmdNextPrevSection(el, true);
    } else if (cmd == u"prev-section") {
        if (!(el = box)) {
            el = cr;
        }
        el = cmdNextPrevSection(el, false);
    } else if (cmd == u"next-track" && cr) {
        el = nextTrack(cr);
        if (noteEntryMode()) {
            _is.moveInputPos(el);
        }
    } else if (cmd == u"prev-track" && cr) {
        el = prevTrack(cr);
        if (noteEntryMode()) {
            _is.moveInputPos(el);
        }
    } else if (cmd == u"top-staff") {
        el = cr ? cmdTopStaff(cr) : cmdTopStaff();
        if (noteEntryMode()) {
            _is.moveInputPos(el);
        }
    } else if (cmd == u"empty-trailing-measure") {
        const Measure* ftm = nullptr;
        if (!cr) {
            ftm = firstTrailingMeasure() ? firstTrailingMeasure() : lastMeasure();
        } else {
            ftm = firstTrailingMeasure(&cr) ? firstTrailingMeasure(&cr) : lastMeasure();
        }
        if (ftm) {
            if (score()->styleB(Sid::createMultiMeasureRests) && ftm->hasMMRest()) {
                ftm = ftm->mmRest1();
            }
            el = !cr ? ftm->first()->nextChordRest(0, false) : ftm->first()->nextChordRest(trackZeroVoice(cr->track()), false);
        }
        // Note: Due to the nature of this command as being preparatory for input,
        // Note-Entry is activated from within ScoreView::cmd()
        _is.moveInputPos(el);
    }

    if (el) {
        if (el->type() == ElementType::CHORD) {
            el = toChord(el)->upNote();             // originally downNote
        }
        setPlayNote(true);
        if (noteEntryMode()) {
            // if cursor moved into a gap, selection cannot follow
            // only select & play el if it was not already selected (does not normally happen)
            if (_is.cr() || !el->selected()) {
                select(el, SelectType::SINGLE, 0);
            } else {
                setPlayNote(false);
            }
            for (MuseScoreView* view : viewer) {
                view->moveCursor();
            }
        } else {
            select(el, SelectType::SINGLE, 0);
        }
    }
    return el;
}

//---------------------------------------------------------
//   selectMove
//---------------------------------------------------------

EngravingItem* Score::selectMove(const String& cmd)
{
    ChordRest* cr;
    if (selection().activeCR()) {
        cr = selection().activeCR();
    } else {
        cr = selection().lastChordRest();
    }
    if (!cr && noteEntryMode()) {
        cr = inputState().cr();
    }
    if (!cr) {
        return 0;
    }

    ChordRest* el = nullptr;
    if (cmd == u"select-next-chord") {
        el = nextChordRest(cr, true, false);
    } else if (cmd == u"select-prev-chord") {
        el = prevChordRest(cr, true, false);
    } else if (cmd == u"select-next-measure") {
        el = nextMeasure(cr, true, true);
    } else if (cmd == u"select-prev-measure") {
        el = prevMeasure(cr, true);
    } else if (cmd == u"select-begin-line") {
        Measure* measure = cr->segment()->measure()->system()->firstMeasure();
        if (!measure) {
            return 0;
        }
        el = measure->first()->nextChordRest(cr->track());
    } else if (cmd == u"select-end-line") {
        Measure* measure = cr->segment()->measure()->system()->lastMeasure();
        if (!measure) {
            return 0;
        }
        el = measure->last()->nextChordRest(cr->track(), true);
    } else if (cmd == u"select-begin-score") {
        Measure* measure = firstMeasureMM();
        if (!measure) {
            return 0;
        }
        el = measure->first()->nextChordRest(cr->track());
    } else if (cmd == u"select-end-score") {
        Measure* measure = lastMeasureMM();
        if (!measure) {
            return 0;
        }
        el = measure->last()->nextChordRest(cr->track(), true);
    } else if (cmd == u"select-staff-above") {
        el = upStaff(cr);
    } else if (cmd == u"select-staff-below") {
        el = downStaff(cr);
    }
    if (el) {
        select(el, SelectType::RANGE, el->staffIdx());
    }
    return el;
}

//---------------------------------------------------------
//   cmdMirrorNoteHead
//---------------------------------------------------------

void Score::cmdMirrorNoteHead()
{
    const std::vector<EngravingItem*>& el = selection().elements();
    for (EngravingItem* e : el) {
        if (e->isNote()) {
            Note* note = toNote(e);
            if (note->staff() && note->staff()->isTabStaff(note->chord()->tick())) {
                e->undoChangeProperty(Pid::GHOST, !note->ghost());
            } else {
                DirectionH d = note->userMirror();
                if (d == DirectionH::AUTO) {
                    d = note->chord()->up() ? DirectionH::RIGHT : DirectionH::LEFT;
                } else {
                    d = d == DirectionH::LEFT ? DirectionH::RIGHT : DirectionH::LEFT;
                }
                undoChangeUserMirror(note, d);
            }
        } else if (e->isHairpinSegment()) {
            Hairpin* h = toHairpinSegment(e)->hairpin();
            HairpinType st = h->hairpinType();
            switch (st) {
            case HairpinType::CRESC_HAIRPIN:
                st = HairpinType::DECRESC_HAIRPIN;
                break;
            case HairpinType::DECRESC_HAIRPIN:
                st = HairpinType::CRESC_HAIRPIN;
                break;
            case HairpinType::CRESC_LINE:
                st = HairpinType::DECRESC_LINE;
                break;
            case HairpinType::DECRESC_LINE:
                st = HairpinType::CRESC_LINE;
                break;
            case HairpinType::INVALID:
                break;
            }
            h->undoChangeProperty(Pid::HAIRPIN_TYPE, int(st));
        }
    }
}

//---------------------------------------------------------
//   cmdIncDecDuration
//     When stepDotted is false and nSteps is 1 or -1, will halve or double the duration
//     When stepDotted is true, will step by nearest dotted or undotted note
//---------------------------------------------------------

void Score::cmdIncDecDuration(int nSteps, bool stepDotted)
{
    EngravingItem* el = selection().element();
    if (el == 0) {
        return;
    }
    if (el->isNote()) {
        el = el->parentItem();
    }
    if (!el->isChordRest()) {
        return;
    }

    ChordRest* cr = toChordRest(el);

    // if measure rest is selected as input, then the correct initialDuration will be the
    // duration of the measure's time signature, else is just the input state's duration
    TDuration initialDuration
        = (cr->durationType() == DurationType::V_MEASURE) ? TDuration(cr->measure()->timesig()) : _is.duration();
    TDuration d = initialDuration.shiftRetainDots(nSteps, stepDotted);
    if (!d.isValid()) {
        return;
    }
    if (cr->isChord() && (toChord(cr)->noteType() != NoteType::NORMAL)) {
        //
        // handle appoggiatura and acciaccatura
        //
        undoChangeChordRestLen(cr, d);
    } else {
        changeCRlen(cr, d);
    }
    _is.setDuration(d);
    nextInputPos(cr, false);
}

//---------------------------------------------------------
//   cmdAddBracket
//---------------------------------------------------------

void Score::cmdAddBracket()
{
    for (EngravingItem* el : selection().elements()) {
        if (el->type() == ElementType::ACCIDENTAL) {
            Accidental* acc = toAccidental(el);
            acc->undoChangeProperty(Pid::ACCIDENTAL_BRACKET, int(AccidentalBracket::BRACKET));
        }
    }
}

//---------------------------------------------------------
//   cmdAddParentheses
//---------------------------------------------------------

void Score::cmdAddParentheses()
{
    for (EngravingItem* el : selection().elements()) {
        if (el->type() == ElementType::NOTE) {
            Note* n = toNote(el);
            n->setHeadHasParentheses(true);
        } else if (el->type() == ElementType::ACCIDENTAL) {
            Accidental* acc = toAccidental(el);
            acc->undoChangeProperty(Pid::ACCIDENTAL_BRACKET, int(AccidentalBracket::PARENTHESIS));
        } else if (el->type() == ElementType::HARMONY) {
            Harmony* h = toHarmony(el);
            h->setLeftParen(true);
            h->setRightParen(true);
            h->render();
        } else if (el->type() == ElementType::TIMESIG) {
            TimeSig* ts = toTimeSig(el);
            ts->setLargeParentheses(true);
        }
    }
}

//---------------------------------------------------------
//   cmdAddBraces
//---------------------------------------------------------

void Score::cmdAddBraces()
{
    for (EngravingItem* el : selection().elements()) {
        if (el->type() == ElementType::ACCIDENTAL) {
            Accidental* acc = toAccidental(el);
            acc->undoChangeProperty(Pid::ACCIDENTAL_BRACKET, int(AccidentalBracket::BRACE));
        }
    }
}

//---------------------------------------------------------
//   cmdMoveRest
//---------------------------------------------------------

void Score::cmdMoveRest(Rest* rest, DirectionV dir)
{
    PointF pos(rest->offset());
    if (dir == DirectionV::UP) {
        pos.ry() -= spatium();
    } else if (dir == DirectionV::DOWN) {
        pos.ry() += spatium();
    }
    rest->undoChangeProperty(Pid::OFFSET, pos);
}

//---------------------------------------------------------
//   cmdMoveLyrics
//---------------------------------------------------------

void Score::cmdMoveLyrics(Lyrics* lyrics, DirectionV dir)
{
    int verse = lyrics->no() + (dir == DirectionV::UP ? -1 : 1);
    if (verse < 0) {
        return;
    }
    lyrics->undoChangeProperty(Pid::VERSE, verse);
}

//---------------------------------------------------------
//   realtimeAdvance
//---------------------------------------------------------

void Score::realtimeAdvance()
{
    InputState& is = inputState();
    if (!is.noteEntryMode()) {
        return;
    }

    Fraction ticks2measureEnd = is.segment()->measure()->ticks() - is.segment()->rtick();
    if (!is.cr() || (is.cr()->ticks() != is.duration().fraction() && is.duration() < ticks2measureEnd)) {
        setNoteRest(is.segment(), is.track(), NoteVal(), is.duration().fraction(), DirectionV::AUTO);
    }

    ChordRest* prevCR = toChordRest(is.cr());
    if (inputState().endOfScore()) {
        appendMeasures(1);
    }

    is.moveToNextInputPos();

    std::list<MidiInputEvent>& midiPitches = activeMidiPitches();
    if (midiPitches.empty()) {
        setNoteRest(is.segment(), is.track(), NoteVal(), is.duration().fraction(), DirectionV::AUTO);
    } else {
        Chord* prevChord = prevCR->isChord() ? toChord(prevCR) : 0;
        bool partOfChord = false;
        for (const MidiInputEvent& ev : midiPitches) {
            addTiedMidiPitch(ev.pitch, partOfChord, prevChord);
            partOfChord = true;
        }
    }

    if (prevCR->measure() != is.segment()->measure()) {
        // just advanced across barline. Now simplify tied notes.
        score()->regroupNotesAndRests(prevCR->measure()->tick(), is.segment()->measure()->tick(), is.track());
    }

    return;
}

//---------------------------------------------------------
//   cmdInsertClef
//---------------------------------------------------------

void Score::cmdInsertClef(ClefType type)
{
    undoChangeClef(staff(inputTrack() / VOICES), inputState().cr(), type);
}

//---------------------------------------------------------
//   cmdInsertClef
//    insert clef before cr
//---------------------------------------------------------

void Score::cmdInsertClef(Clef* clef, ChordRest* cr)
{
    undoChangeClef(cr->staff(), cr, clef->clefType());
    delete clef;
}

//---------------------------------------------------------
//   cmdAddGrace
///   adds grace note of specified type to selected notes
//---------------------------------------------------------

void Score::cmdAddGrace(NoteType graceType, int duration)
{
    const std::vector<EngravingItem*> copyOfElements = selection().elements();
    for (EngravingItem* e : copyOfElements) {
        if (e->type() == ElementType::NOTE) {
            Note* n = toNote(e);
            setGraceNote(n->chord(), n->pitch(), graceType, duration);
        }
    }
}

//---------------------------------------------------------
//   cmdAddMeasureRepeat
//---------------------------------------------------------

void Score::cmdAddMeasureRepeat(Measure* firstMeasure, int numMeasures, staff_idx_t staffIdx)
{
    //
    // make measures into group
    //
    if (!makeMeasureRepeatGroup(firstMeasure, numMeasures, staffIdx)) {
        return;
    }

    //
    // add MeasureRepeat element
    //
    int measureWithElementNo;
    if (numMeasures % 2) {
        // odd number, element anchored to center measure of group
        measureWithElementNo = numMeasures / 2 + 1;
    } else {
        // even number, element anchored to last measure in first half of group
        measureWithElementNo = numMeasures / 2;
    }
    Measure* measureWithElement = firstMeasure;
    for (int i = 1; i < measureWithElementNo; ++i) {
        measureWithElement = measureWithElement->nextMeasure();
    }
    // MeasureRepeat element will be positioned appropriately (in center of measure / on following barline)
    // when stretchMeasure() is called on measureWithElement
    MeasureRepeat* mr = addMeasureRepeat(measureWithElement->tick(), staff2track(staffIdx), numMeasures);
    select(mr, SelectType::SINGLE, 0);
}

//---------------------------------------------------------
//   makeMeasureRepeatGroup
///   clear measures, apply noBreak, set measureRepeatCount
///   returns false if these measures won't work or user aborted
//---------------------------------------------------------

bool Score::makeMeasureRepeatGroup(Measure* firstMeasure, int numMeasures, staff_idx_t staffIdx)
{
    //
    // check that sufficient measures exist, with equal durations
    //
    std::vector<Measure*> measures;
    Measure* measure = firstMeasure;
    for (int i = 1; i <= numMeasures; ++i) {
        if (!measure || measure->ticks() != firstMeasure->ticks()) {
            MScore::setError(MsError::INSUFFICIENT_MEASURES);
            return false;
        }
        measures.push_back(measure);
        measure = measure->nextMeasure();
    }

    //
    // warn user if things will have to be deleted to make room for measure repeat
    //
    bool empty = true;
    for (auto m : measures) {
        if (m != measures.back()) {
            if (m->endBarLineType() != BarLineType::NORMAL) {
                empty = false;
                break;
            }
        }
        for (auto seg = m->first(); seg && empty; seg = seg->next()) {
            if (seg->segmentType() & SegmentType::ChordRest) {
                track_idx_t strack = staffIdx * VOICES;
                track_idx_t etrack = strack + VOICES;
                for (track_idx_t track = strack; track < etrack; ++track) {
                    EngravingItem* e = seg->element(track);
                    if (e && !e->generated() && !e->isRest()) {
                        empty = false;
                        break;
                    }
                }
            }
        }
    }

    if (!empty) {
        auto b = MessageBox::warning(trc("engraving", "Current contents of measures will be replaced"),
                                     trc("engraving", "Continue with inserting measure repeat?"));
        if (b == MessageBox::Button::Cancel) {
            return false;
        }
    }

    //
    // group measures and clear current contents
    //

    deselectAll();
    int i = 1;
    for (auto m : measures) {
        select(m, SelectType::RANGE, staffIdx);
        if (m->isMeasureRepeatGroup(staffIdx)) {
            deleteItem(m->measureRepeatElement(staffIdx)); // reset measures related to an earlier MeasureRepeat
        }
        undoChangeMeasureRepeatCount(m, i++, staffIdx);
        if (m != measures.front()) {
            m->undoChangeProperty(Pid::REPEAT_START, false);
        }
        if (m != measures.back()) {
            m->undoSetNoBreak(true);
            Segment* seg = m->findSegmentR(SegmentType::EndBarLine, m->ticks());
            BarLine* endBarLine = toBarLine(seg->element(staff2track(staffIdx)));
            deleteItem(endBarLine); // also takes care of Pid::REPEAT_END
        }
    }
    cmdDeleteSelection();
    return true;
}

//---------------------------------------------------------
//   cmdExplode
///   explodes contents of top selected staff into subsequent staves
//---------------------------------------------------------

void Score::cmdExplode()
{
    size_t srcStaff  = selection().staffStart();
    size_t lastStaff = selection().staffEnd();
    size_t srcTrack  = srcStaff * VOICES;

    // reset selection to top staff only
    // force complete measures
    Segment* startSegment = selection().startSegment();
    Segment* endSegment = selection().endSegment();
    Measure* startMeasure = startSegment->measure();
    Measure* endMeasure = endSegment ? endSegment->measure() : lastMeasure();

    Fraction lTick = endMeasure->endTick();
    bool voice = false;

    for (Measure* m = startMeasure; m && m->tick() != lTick; m = m->nextMeasure()) {
        if (m->hasVoices(srcStaff)) {
            voice = true;
            break;
        }
    }
    if (!voice) {
        // force complete measures
        deselectAll();
        select(startMeasure, SelectType::RANGE, srcStaff);
        select(endMeasure, SelectType::RANGE, srcStaff);
        startSegment = selection().startSegment();
        endSegment = selection().endSegment();
        if (srcStaff == lastStaff - 1) {
            // only one staff was selected up front - determine number of staves
            // loop through all chords looking for maximum number of notes
            int n = 0;
            for (Segment* s = startSegment; s && s != endSegment; s = s->next1()) {
                EngravingItem* e = s->element(srcTrack);
                if (e && e->type() == ElementType::CHORD) {
                    Chord* c = toChord(e);
                    n = std::max(n, int(c->notes().size()));
                }
            }
            lastStaff = std::min(nstaves(), srcStaff + n);
        }

        const ByteArray mimeData(selection().mimeData());
        // copy to all destination staves
        Segment* firstCRSegment = startMeasure->tick2segment(startMeasure->tick());
        for (size_t i = 1; srcStaff + i < lastStaff; ++i) {
            track_idx_t track = (srcStaff + i) * VOICES;
            ChordRest* cr = toChordRest(firstCRSegment->element(track));
            if (cr) {
                XmlReader e(mimeData);
                e.context()->setPasteMode(true);
                pasteStaff(e, cr->segment(), cr->staffIdx());
            }
        }

        // loop through each staff removing all but one note from each chord
        for (size_t i = 0; srcStaff + i < lastStaff; ++i) {
            track_idx_t track = (srcStaff + i) * VOICES;
            for (Segment* s = startSegment; s && s != endSegment; s = s->next1()) {
                EngravingItem* e = s->element(track);
                if (e && e->type() == ElementType::CHORD) {
                    Chord* c = toChord(e);
                    std::vector<Note*> notes = c->notes();
                    size_t nnotes = notes.size();
                    // keep note "i" from top, which is backwards from nnotes - 1
                    // reuse notes if there are more instruments than notes
                    size_t stavesPerNote = std::max((lastStaff - srcStaff) / nnotes, static_cast<size_t>(1));
                    size_t keepIndex = std::max(nnotes - 1 - (i / stavesPerNote), static_cast<size_t>(0));
                    Note* keepNote = c->notes()[keepIndex];
                    for (Note* n : notes) {
                        if (n != keepNote) {
                            undoRemoveElement(n);
                        }
                    }
                }
            }
        }
    } else {
        track_idx_t sTracks[VOICES];
        track_idx_t dTracks[VOICES];
        if (srcStaff == lastStaff - 1) {
            lastStaff = std::min(nstaves(), srcStaff + VOICES);
        }

        for (voice_idx_t i = 0; i < VOICES; i++) {
            sTracks[i] = mu::nidx;
            dTracks[i] = mu::nidx;
        }
        int full = 0;

        for (Segment* seg = startSegment; seg && seg->tick() < lTick; seg = seg->next1()) {
            for (track_idx_t i = srcTrack; i < srcTrack + VOICES && full != VOICES; i++) {
                bool t = true;
                for (voice_idx_t j = 0; j < VOICES; j++) {
                    if (i == sTracks[j]) {
                        t = false;
                        break;
                    }
                }

                if (!seg->measure()->hasVoice(i) || seg->measure()->isOnlyRests(i) || !t) {
                    continue;
                }
                sTracks[full] = i;

                for (size_t j = srcTrack + full * VOICES; j < lastStaff * VOICES; j++) {
                    if (i == j) {
                        dTracks[full] = j;
                        break;
                    }
                    for (Measure* m = seg->measure(); m && m->tick() < lTick; m = m->nextMeasure()) {
                        if (!m->hasVoice(j) || (m->hasVoice(j) && m->isOnlyRests(j))) {
                            dTracks[full] = j;
                        } else {
                            dTracks[full] = mu::nidx;
                            break;
                        }
                    }
                    if (dTracks[full] != mu::nidx) {
                        break;
                    }
                }
                full++;
            }
        }

        for (track_idx_t i = srcTrack, j = 0; i < lastStaff * VOICES && j < VOICES; i += VOICES, j++) {
            track_idx_t strack = sTracks[j % VOICES];
            track_idx_t dtrack = dTracks[j % VOICES];
            if (strack != mu::nidx && strack != dtrack && dtrack != mu::nidx) {
                undo(new CloneVoice(startSegment, lTick, startSegment, strack, dtrack, mu::nidx, false));
            }
        }
    }

    // select exploded region
    deselectAll();
    select(startMeasure, SelectType::RANGE, srcStaff);
    select(endMeasure, SelectType::RANGE, lastStaff - 1);
}

//---------------------------------------------------------
//   cmdImplode
///   implodes contents of selected staves into top staff
///   for single staff, merge voices
//---------------------------------------------------------

void Score::cmdImplode()
{
    staff_idx_t dstStaff   = selection().staffStart();
    staff_idx_t endStaff   = selection().staffEnd();
    track_idx_t dstTrack   = dstStaff * VOICES;
    track_idx_t startTrack = dstStaff * VOICES;
    track_idx_t endTrack   = endStaff * VOICES;
    Segment* startSegment = selection().startSegment();
    Segment* endSegment = selection().endSegment();
    Measure* startMeasure = startSegment->measure();
    Measure* endMeasure = endSegment ? endSegment->measure() : lastMeasure();
    Fraction startTick       = startSegment->tick();
    Fraction endTick         = endSegment ? endSegment->tick() : lastMeasure()->endTick();
    assert(startMeasure && endMeasure);

    // if single staff selected, combine voices
    // otherwise combine staves
    if (dstStaff == endStaff - 1) {
        // loop through segments adding notes to chord on top staff
        for (Segment* s = startSegment; s && s != endSegment; s = s->next1()) {
            if (!s->isChordRestType()) {
                continue;
            }
            EngravingItem* dst = s->element(dstTrack);
            if (dst && dst->isChord()) {
                Chord* dstChord = toChord(dst);
                // see if we are tying in to this chord
                Chord* tied = 0;
                for (Note* n : dstChord->notes()) {
                    if (n->tieBack()) {
                        tied = n->tieBack()->startNote()->chord();
                        break;
                    }
                }
                // loop through each subsequent staff (or track within staff)
                // looking for notes to add
                for (track_idx_t srcTrack = startTrack + 1; srcTrack < endTrack; srcTrack++) {
                    EngravingItem* src = s->element(srcTrack);
                    if (src && src->isChord()) {
                        Chord* srcChord = toChord(src);
                        // when combining voices, skip if not same duration
                        if (srcChord->ticks() != dstChord->ticks()) {
                            continue;
                        }
                        // add notes
                        for (Note* n : srcChord->notes()) {
                            NoteVal nv(n->pitch());
                            nv.tpc1 = n->tpc1();
                            // skip duplicates
                            if (dstChord->findNote(nv.pitch)) {
                                continue;
                            }
                            Note* nn = addNote(dstChord, nv);
                            // add tie to this note if original chord was tied
                            if (tied) {
                                // find note to tie to
                                for (Note* tn : tied->notes()) {
                                    if (nn->pitch() == tn->pitch() && nn->tpc() == tn->tpc() && !tn->tieFor()) {
                                        // found note to tie
                                        Tie* tie = Factory::createTie(this->dummy());
                                        tie->setStartNote(tn);
                                        tie->setEndNote(nn);
                                        tie->setTick(tie->startNote()->tick());
                                        tie->setTick2(tie->endNote()->tick());
                                        tie->setTrack(tn->track());
                                        undoAddElement(tie);
                                    }
                                }
                            }
                        }
                    }
                    // delete chordrest from source track if possible
                    if (src && src->voice()) {
                        undoRemoveElement(src);
                    }
                }
            }
            // TODO - use first voice that actually has a note and implode remaining voices on it?
            // see https://musescore.org/en/node/174111
            else if (dst) {
                // destination track has something, but it isn't a chord
                // remove rests from other voices if in "voice mode"
                for (voice_idx_t i = 1; i < VOICES; ++i) {
                    EngravingItem* e = s->element(dstTrack + i);
                    if (e && e->isRest()) {
                        undoRemoveElement(e);
                    }
                }
            }
        }
        // delete orphaned spanners (TODO: figure out solution to reconnect orphaned spanners to their cloned notes)
        checkSpanner(startTick, endTick);
    } else {
        track_idx_t tracks[VOICES];
        for (voice_idx_t i = 0; i < VOICES; i++) {
            tracks[i] = mu::nidx;
        }
        voice_idx_t full = 0;

        // identify tracks to combine, storing the source track numbers in tracks[]
        // first four non-empty tracks to win
        for (track_idx_t track = startTrack; track < endTrack && full < VOICES; ++track) {
            Measure* m = startMeasure;
            do {
                if (m->hasVoice(track) && !m->isOnlyRests(track)) {
                    tracks[full++] = track;
                    break;
                }
            } while ((m != endMeasure) && (m = m->nextMeasure()));
        }

        // clone source tracks into destination
        for (track_idx_t i = dstTrack; i < dstTrack + VOICES; i++) {
            track_idx_t strack = tracks[i % VOICES];
            if (strack != mu::nidx && strack != i) {
                undo(new CloneVoice(startSegment, endTick, startSegment, strack, i, i, false));
            }
        }
    }

    // select destination staff only
    deselectAll();
    select(startMeasure, SelectType::RANGE, dstStaff);
    select(endMeasure, SelectType::RANGE, dstStaff);
}

//---------------------------------------------------------
//   cmdSlashFill
///   fills selected region with slashes
//---------------------------------------------------------

void Score::cmdSlashFill()
{
    staff_idx_t startStaff = selection().staffStart();
    staff_idx_t endStaff = selection().staffEnd();
    Segment* startSegment = selection().startSegment();
    if (!startSegment) { // empty score?
        return;
    }

    Segment* endSegment = selection().endSegment();

    // operate on measures underlying mmrests
    if (startSegment && startSegment->measure() && startSegment->measure()->isMMRest()) {
        startSegment = startSegment->measure()->mmRestFirst()->first();
    }
    if (endSegment && endSegment->measure() && endSegment->measure()->isMMRest()) {
        endSegment = endSegment->measure()->mmRestLast()->last();
    }

    Fraction endTick = endSegment ? endSegment->tick() : lastSegment()->tick() + Fraction::fromTicks(1);
    Chord* firstSlash = 0;
    Chord* lastSlash = 0;

    // loop through staves in selection
    for (staff_idx_t staffIdx = startStaff; staffIdx < endStaff; ++staffIdx) {
        track_idx_t track = staffIdx * VOICES;
        voice_idx_t voice = mu::nidx;
        // loop through segments adding slashes on each beat
        for (Segment* s = startSegment; s && s->tick() < endTick; s = s->next1()) {
            if (s->segmentType() != SegmentType::ChordRest) {
                continue;
            }
            // determine beat type based on time signature
            int d = s->measure()->timesig().denominator();
            int n = (d > 4 && s->measure()->timesig().numerator() % 3 == 0) ? 3 : 1;
            Fraction f(n, d);
            // skip over any leading segments before next (first) beat
            if (s->rtick().ticks() % f.ticks()) {
                continue;
            }
            // determine voice to use - first available voice for this measure / staff
            if (voice == mu::nidx || s->rtick().isZero()) {
                bool needGap[VOICES];
                for (voice = 0; voice < VOICES; ++voice) {
                    needGap[voice] = false;
                    ChordRest* cr = toChordRest(s->element(track + voice));
                    // no chordrest == treat as ordinary rest for purpose of determining availability of voice
                    // but also, we will need to make a gap for this voice if we do end up choosing it
                    if (!cr) {
                        needGap[voice] = true;
                    }
                    // chord == keep looking for an available voice
                    else if (cr->type() == ElementType::CHORD) {
                        continue;
                    }
                    // full measure rest == OK to use voice
                    else if (cr->durationType() == DurationType::V_MEASURE) {
                        break;
                    }
                    // no chordrest or ordinary rest == OK to use voice
                    // if there are nothing but rests for duration of measure / selection
                    bool ok = true;
                    for (Segment* ns = s->next(SegmentType::ChordRest); ns && ns != endSegment; ns = ns->next(SegmentType::ChordRest)) {
                        ChordRest* ncr = toChordRest(ns->element(track + voice));
                        if (ncr && ncr->type() == ElementType::CHORD) {
                            ok = false;
                            break;
                        }
                    }
                    if (ok) {
                        break;
                    }
                }
                // no available voices, just use voice 0
                if (voice == VOICES) {
                    voice = 0;
                }
                // no cr was found in segment for this voice, so make gap
                if (needGap[voice]) {
                    makeGapVoice(s, track + voice, f, s->tick());
                }
            }
            // construct note
            int line = 0;
            bool error = false;
            NoteVal nv;
            if (staff(staffIdx)->staffType(s->tick())->group() == StaffGroup::TAB) {
                line = staff(staffIdx)->lines(s->tick()) / 2;
            } else {
                line = staff(staffIdx)->middleLine(s->tick());             // staff(staffIdx)->lines() - 1;
            }
            if (staff(staffIdx)->staffType(s->tick())->group() == StaffGroup::PERCUSSION) {
                nv.pitch = 0;
                nv.headGroup = NoteHeadGroup::HEAD_SLASH;
            } else {
                Position p;
                p.segment = s;
                p.staffIdx = staffIdx;
                p.line = line;
                p.fret = INVALID_FRET_INDEX;
                _is.setRest(false);             // needed for tab
                nv = noteValForPosition(p, AccidentalType::NONE, error);
            }
            if (error) {
                continue;
            }
            // insert & turn into slash
            s = setNoteRest(s, track + voice, nv, f);
            Chord* c = toChord(s->element(track + voice));
            if (c) {
                if (c->links()) {
                    for (EngravingObject* e : *c->links()) {
                        Chord* lc = toChord(e);
                        lc->setSlash(true, true);
                    }
                } else {
                    c->setSlash(true, true);
                }
            }
            lastSlash = c;
            if (!firstSlash) {
                firstSlash = c;
            }
        }
    }

    // re-select the slashes
    deselectAll();
    if (firstSlash && lastSlash) {
        select(firstSlash, SelectType::RANGE);
        select(lastSlash, SelectType::RANGE);
    }
}

//---------------------------------------------------------
//   cmdSlashRhythm
///   converts rhythms in selected region to slashes
//---------------------------------------------------------

void Score::cmdSlashRhythm()
{
    std::set<Chord*> chords;
    // loop through all notes in selection
    for (EngravingItem* e : selection().elements()) {
        if (e->voice() >= 2 && e->isRest()) {
            Rest* r = toRest(e);
            if (r->links()) {
                for (EngravingObject* se : *r->links()) {
                    Rest* lr = toRest(se);
                    lr->setAccent(!lr->accent());
                }
            } else {
                r->setAccent(!r->accent());
            }
            continue;
        } else if (e->isNote()) {
            Note* n = toNote(e);
            if (n->noteType() != NoteType::NORMAL) {
                continue;
            }
            Chord* c = n->chord();
            // check for duplicates (chords with multiple notes)
            if (mu::contains(chords, c)) {
                continue;
            }
            chords.insert(c);
            // toggle slash setting
            if (c->links()) {
                for (EngravingObject* se : *c->links()) {
                    Chord* lc = toChord(se);
                    lc->setSlash(!lc->slash(), false);
                }
            } else {
                c->setSlash(!c->slash(), false);
            }
        }
    }
}

//---------------------------------------------------------
//   cmdRealizeChordSymbols
///   Realize selected chord symbols into notes on the staff.
///
///   If a voicing and duration type are specified, the
///   harmony voicing settings will be overridden by the
///   passed parameters. Otherwise, the settings set on the
///   harmony object will be used.
//---------------------------------------------------------

void Score::cmdRealizeChordSymbols(bool literal, Voicing voicing, HDuration durationType)
{
    const std::vector<EngravingItem*>& elist = selection().elements();
    for (EngravingItem* e : elist) {
        if (!e->isHarmony()) {
            continue;
        }
        Harmony* h = toHarmony(e);
        if (!h->isRealizable()) {
            continue;
        }
        const RealizedHarmony& r = h->getRealizedHarmony();
        Segment* seg = h->explicitParent()->isSegment() ? toSegment(h->explicitParent()) : toSegment(h->explicitParent()->explicitParent());
        Fraction tick = seg->tick();
        Fraction duration = r.getActualDuration(tick.ticks(), durationType);
        bool concertPitch = styleB(Sid::concertPitch);

        Chord* chord = Factory::createChord(this->dummy()->segment());     //chord template
        chord->setTrack(h->track());     //set track so notes have a track to sit on

        //create chord from notes
        RealizedHarmony::PitchMap notes;
        if (voicing == Voicing::INVALID || durationType == HDuration::INVALID) {
            notes = r.notes();       //no override, just use notes from realize harmony
        } else {
            //generate notes list based on overridden settings
            int offset = 0;
            Interval interval = h->staff()->part()->instrument(h->tick())->transpose();
            if (!concertPitch) {
                offset = interval.chromatic;
            }
            notes = r.generateNotes(h->rootTpc(), h->baseTpc(),
                                    literal, voicing, offset);
        }

        for (const auto& p : notes) {
            Note* note = Factory::createNote(chord);
            NoteVal nval;
            nval.pitch = p.first;
            if (concertPitch) {
                nval.tpc1 = p.second;
            } else {
                nval.tpc2 = p.second;
            }
            chord->add(note);       //add note first to set track and such
            note->setNval(nval, tick);
        }

        setChord(seg, h->track(), chord, duration);     //add chord using template
        delete chord;
    }
}

//---------------------------------------------------------
//   setChord
//    return segment of last created chord
//---------------------------------------------------------
Segment* Score::setChord(Segment* segment, track_idx_t track, const Chord* chordTemplate, Fraction dur, DirectionV stemDirection)
{
    assert(segment->segmentType() == SegmentType::ChordRest);

    Fraction tick = segment->tick();
    Chord* nr     = nullptr;   //current added chord used so we can select the last added chord and so we can apply ties
    std::vector<Tie*> tie(chordTemplate->notes().size());   //keep pointer to a tie for each note in the chord in case we need to tie notes
    ChordRest* cr = toChordRest(segment->element(track));   //chord rest under the segment for the specified track

    bool addTie = false;

    Measure* measure = nullptr;
    //keep creating chords and tieing them until we created the full duration asked for (dur)
    for (;;) {
        if (track % VOICES) {
            expandVoice(segment, track);
        }

        Tuplet* t = cr ? cr->tuplet() : 0;
        Fraction tDur = segment->ticks();
        Segment* seg = segment->next();

        //we need to get a correct subduration so that makeGap can function properly
        //since makeGap() takes "normal" duration rather than actual length
        while (seg) {
            if (seg->segmentType() == SegmentType::ChordRest) {
                //design choice made to keep multiple notes across a tuplet as tied single notes rather than combining them
                //since it's arguably more readable, but the other code is still here (commented)
                ChordRest* testCr = toChordRest(seg->element(track));

                //code here allows us to combine tuplet realization together which I have opted not to do for readability (of the music)
                //if (!!t ^ (testCr && testCr->tuplet())) //stop if we started with a tuplet and reach something that's not a tuplet,
                //      break;                          //or start with not a tuplet and reach a tuplet

                if (testCr && testCr->tuplet()) {       //stop on tuplet
                    break;
                }
                tDur += seg->ticks();
            }
            if (tDur >= dur) {       //do not go further than the duration asked for
                tDur = dur;
                break;
            }
            seg = seg->next();       //iterate only across measure (hence usage of next() rather than next1())
        }
        if (t) {
            tDur *= t->ratio();       //scale by tuplet ratio to get "normal" length rather than actual length when dealing with tuplets
        }
        // the returned gap ends at the measure boundary or at tuplet end
        Fraction dd = makeGap(segment, track, tDur, t);

        if (dd.isZero()) {
            LOGD("cannot get gap at %d type: %d/%d", tick.ticks(), dur.numerator(),
                 dur.denominator());
            break;
        }

        measure = segment->measure();
        std::vector<TDuration> dl = toDurationList(dd, true);
        size_t n = dl.size();
        //add chord, tieing when necessary within measure
        for (size_t i = 0; i < n; ++i) {
            const TDuration& d = dl[i];

            //create new chord from template and add it
            Chord* chord = Factory::copyChord(*chordTemplate);
            nr = chord;

            chord->setTrack(track);
            chord->setDurationType(d);
            chord->setTicks(d.fraction());
            chord->setStemDirection(stemDirection);
            chord->setTuplet(t);
            undoAddCR(chord, measure, tick);
            //if there is something to tie, complete tie backwards
            //and add the tie to score
            const std::vector<Note*> notes = chord->notes();
            if (addTie) {
                for (size_t j = 0; j < notes.size(); ++j) {
                    tie[j]->setEndNote(notes[j]);
                    notes[j]->setTieBack(tie[j]);
                    undoAddElement(tie[j]);
                }
                addTie = false;
            }
            //if we're not the last element in the duration list,
            //set tie forward
            if (i + 1 < n) {
                for (size_t j = 0; j < notes.size(); ++j) {
                    tie[j] = Factory::createTie(this->dummy());
                    tie[j]->setStartNote(notes[j]);
                    tie[j]->setTick(tie[j]->startNote()->tick());
                    tie[j]->setTrack(track);
                    notes[j]->setTieFor(tie[j]);
                    addTie = true;
                }
            }
            setPlayChord(true);
            segment = chord->segment();
            tick += chord->actualTicks();
        }

        //subtract the duration already realized and move on
        if (t) {
            dur -= dd / t->ratio();
        } else {
            dur -= dd;
        }
        //we are done when there is no duration left to realize
        if (dur.isZero()) {
            break;
        }

        //go to next segment unless we are at the score (which means we will just be done there)
        Segment* nseg = tick2segment(tick, false, SegmentType::ChordRest);
        if (!nseg) {
            LOGD("reached end of score");
            break;
        }
        segment = nseg;

        cr = toChordRest(segment->element(track));

        if (!cr) {
            if (track % VOICES) {
                cr = addRest(segment, track, TDuration(DurationType::V_MEASURE), 0);
            } else {
                LOGD("no rest in voice 0");
                break;
            }
        }
        //
        //  Note does not fit on current measure, create Tie to
        //  next part of note
        std::vector<Note*> notes = nr->notes();
        for (size_t i = 0; i < notes.size(); ++i) {
            tie[i] = Factory::createTie(this->dummy());
            tie[i]->setStartNote(notes[i]);
            tie[i]->setTick(tie[i]->startNote()->tick());
            tie[i]->setTrack(notes[i]->track());
            notes[i]->setTieFor(tie[i]);
        }
    }
    if (!tie.empty()) {
        connectTies();
    }
    if (nr) {
        select(nr, SelectType::SINGLE, 0);
    }
    return segment;
}

//---------------------------------------------------------
//   cmdResequenceRehearsalMarks
///   resequences rehearsal marks within a range selection
///   or, if nothing is selected, the entire score
//---------------------------------------------------------

void Score::cmdResequenceRehearsalMarks()
{
    bool noSelection = !selection().isRange();

    if (noSelection) {
        cmdSelectAll();
    } else if (!selection().isRange()) {
        return;
    }

    RehearsalMark* last = 0;
    for (Segment* s = selection().startSegment(); s && s != selection().endSegment(); s = s->next1()) {
        for (EngravingItem* e : s->annotations()) {
            if (e->type() == ElementType::REHEARSAL_MARK) {
                RehearsalMark* rm = toRehearsalMark(e);
                if (last) {
                    String rmText = nextRehearsalMarkText(last, rm);
                    for (EngravingObject* le : rm->linkList()) {
                        le->undoChangeProperty(Pid::TEXT, rmText);
                    }
                }
                last = rm;
            }
        }
    }

    if (noSelection) {
        deselectAll();
    }
}

//---------------------------------------------------------
//   addRemoveBreaks
//    interval lock
//    0        false    remove all linebreaks
//    > 0      false    add linebreak every interval measure
//    d.c.     true     add linebreak at every system end
//---------------------------------------------------------

void Score::addRemoveBreaks(int interval, bool lock)
{
    Segment* startSegment = selection().startSegment();
    if (!startSegment) { // empty score?
        return;
    }
    Segment* endSegment   = selection().endSegment();
    Measure* startMeasure = startSegment->measure();
    Measure* endMeasure   = endSegment ? endSegment->measure() : lastMeasureMM();
    Measure* lastMeasure  = lastMeasureMM();

    // loop through measures in selection
    // count mmrests as a single measure
    int count = 0;
    for (Measure* mm = startMeasure; mm; mm = mm->nextMeasureMM()) {
        // even though we are counting mmrests as a single measure,
        // we need to find last real measure within mmrest for the actual break
        Measure* m = mm->isMMRest() ? mm->mmRestLast() : mm;

        if (lock) {
            // skip last measure of score
            if (mm == lastMeasure) {
                break;
            }
            // skip if it already has a break
            if (m->lineBreak() || m->pageBreak()) {
                continue;
            }
            // add break if last measure of system
            if (mm->system() && mm->system()->lastMeasure() == mm) {
                m->undoSetLineBreak(true);
            }
        } else {
            if (interval == 0) {
                // remove line break if present
                if (m->lineBreak()) {
                    m->undoSetLineBreak(false);
                }
            } else {
                if (++count == interval) {
                    // skip last measure of score
                    if (mm == lastMeasure) {
                        break;
                    }
                    // found place for break; add if not already one present
                    if (!(m->lineBreak() || m->pageBreak())) {
                        m->undoSetLineBreak(true);
                    }
                    // reset count
                    count = 0;
                } else if (m->lineBreak()) {
                    // remove line break if present in wrong place
                    m->undoSetLineBreak(false);
                }
            }
        }

        if (mm == endMeasure) {
            break;
        }
    }
}

//---------------------------------------------------------
//   cmdRemoveEmptyTrailingMeasures
//---------------------------------------------------------

void Score::cmdRemoveEmptyTrailingMeasures()
{
    auto beginMeasure = firstTrailingMeasure();
    if (beginMeasure) {
        deleteMeasures(beginMeasure, lastMeasure());
    }
}

//---------------------------------------------------------
//   cmdPitchUp
//---------------------------------------------------------

void Score::cmdPitchUp()
{
    EngravingItem* el = selection().element();
    if (el && el->isLyrics()) {
        cmdMoveLyrics(toLyrics(el), DirectionV::UP);
    } else if (el && (el->isArticulation() || el->isTextBase())) {
        el->undoChangeProperty(Pid::OFFSET, el->offset() + PointF(0.0, -MScore::nudgeStep * el->spatium()), PropertyFlags::UNSTYLED);
    } else if (el && el->isRest()) {
        cmdMoveRest(toRest(el), DirectionV::UP);
    } else {
        upDown(true, UpDownMode::CHROMATIC);
    }
}

//---------------------------------------------------------
//   cmdPitchDown
//---------------------------------------------------------

void Score::cmdPitchDown()
{
    EngravingItem* el = selection().element();
    if (el && el->isLyrics()) {
        cmdMoveLyrics(toLyrics(el), DirectionV::DOWN);
    } else if (el && (el->isArticulation() || el->isTextBase())) {
        el->undoChangeProperty(Pid::OFFSET, PropertyValue::fromValue(el->offset() + PointF(0.0, MScore::nudgeStep * el->spatium())),
                               PropertyFlags::UNSTYLED);
    } else if (el && el->isRest()) {
        cmdMoveRest(toRest(el), DirectionV::DOWN);
    } else {
        upDown(false, UpDownMode::CHROMATIC);
    }
}

//---------------------------------------------------------
//   cmdTimeDelete
//---------------------------------------------------------

void Score::cmdTimeDelete()
{
    EngravingItem* e = selection().element();
    if (e && e->isBarLine() && toBarLine(e)->segment()->isEndBarLineType()) {
        Measure* m = toBarLine(e)->segment()->measure();
        cmdJoinMeasure(m, m->nextMeasure());
    } else {
        if (_is.insertMode()) {
            globalTimeDelete();
        } else {
            localTimeDelete();
        }
    }
}

//---------------------------------------------------------
//   cmdPitchUpOctave
//---------------------------------------------------------

void Score::cmdPitchUpOctave()
{
    EngravingItem* el = selection().element();
    if (el && (el->isArticulation() || el->isTextBase())) {
        el->undoChangeProperty(Pid::OFFSET,
                               PropertyValue::fromValue(el->offset() + PointF(0.0, -MScore::nudgeStep10 * el->spatium())),
                               PropertyFlags::UNSTYLED);
    } else {
        upDown(true, UpDownMode::OCTAVE);
    }
}

//---------------------------------------------------------
//   cmdPitchDownOctave
//---------------------------------------------------------

void Score::cmdPitchDownOctave()
{
    EngravingItem* el = selection().element();
    if (el && (el->isArticulation() || el->isTextBase())) {
        el->undoChangeProperty(Pid::OFFSET, el->offset() + PointF(0.0, MScore::nudgeStep10 * el->spatium()), PropertyFlags::UNSTYLED);
    } else {
        upDown(false, UpDownMode::OCTAVE);
    }
}

//---------------------------------------------------------
//   cmdPadNoteIncreaseTAB
//---------------------------------------------------------

void Score::cmdPadNoteIncreaseTAB(const EditData& ed)
{
    switch (_is.duration().type()) {
// cycle back from longest to shortest?
//          case TDuration::V_LONG:
//                padToggle(Pad::NOTE128, ed);
//                break;
    case DurationType::V_BREVE:
        padToggle(Pad::NOTE00, ed);
        break;
    case DurationType::V_WHOLE:
        padToggle(Pad::NOTE0, ed);
        break;
    case DurationType::V_HALF:
        padToggle(Pad::NOTE1, ed);
        break;
    case DurationType::V_QUARTER:
        padToggle(Pad::NOTE2, ed);
        break;
    case DurationType::V_EIGHTH:
        padToggle(Pad::NOTE4, ed);
        break;
    case DurationType::V_16TH:
        padToggle(Pad::NOTE8, ed);
        break;
    case DurationType::V_32ND:
        padToggle(Pad::NOTE16, ed);
        break;
    case DurationType::V_64TH:
        padToggle(Pad::NOTE32, ed);
        break;
    case DurationType::V_128TH:
        padToggle(Pad::NOTE64, ed);
        break;
    default:
        break;
    }
}

//---------------------------------------------------------
//   cmdPadNoteDecreaseTAB
//---------------------------------------------------------

void Score::cmdPadNoteDecreaseTAB(const EditData& ed)
{
    switch (_is.duration().type()) {
    case DurationType::V_LONG:
        padToggle(Pad::NOTE0, ed);
        break;
    case DurationType::V_BREVE:
        padToggle(Pad::NOTE1, ed);
        break;
    case DurationType::V_WHOLE:
        padToggle(Pad::NOTE2, ed);
        break;
    case DurationType::V_HALF:
        padToggle(Pad::NOTE4, ed);
        break;
    case DurationType::V_QUARTER:
        padToggle(Pad::NOTE8, ed);
        break;
    case DurationType::V_EIGHTH:
        padToggle(Pad::NOTE16, ed);
        break;
    case DurationType::V_16TH:
        padToggle(Pad::NOTE32, ed);
        break;
    case DurationType::V_32ND:
        padToggle(Pad::NOTE64, ed);
        break;
    case DurationType::V_64TH:
        padToggle(Pad::NOTE128, ed);
        break;
    case DurationType::V_128TH:
        padToggle(Pad::NOTE256, ed);
        break;
    case DurationType::V_256TH:
        padToggle(Pad::NOTE512, ed);
        break;
    case DurationType::V_512TH:
        padToggle(Pad::NOTE1024, ed);
        break;
// cycle back from shortest to longest?
//          case DurationType::V_1024TH:
//                padToggle(Pad::NOTE00, ed);
//                break;
    default:
        break;
    }
}

//---------------------------------------------------------
//   cmdToggleLayoutBreak
//---------------------------------------------------------

void Score::cmdToggleLayoutBreak(LayoutBreakType type)
{
    // find measure(s)
    std::list<MeasureBase*> mbl;
    bool allNoBreaks = true; // NOBREAK is not removed unless every measure in selection already has one
    if (selection().isRange()) {
        Measure* startMeasure = nullptr;
        Measure* endMeasure = nullptr;
        if (!selection().measureRange(&startMeasure, &endMeasure)) {
            return;
        }
        if (!startMeasure || !endMeasure) {
            return;
        }
        if (type == LayoutBreakType::NOBREAK) {
            // add throughout the selection
            // or remove if already on every measure
            if (startMeasure == endMeasure) {
                mbl.push_back(startMeasure);
                allNoBreaks = startMeasure->noBreak();
            } else {
                for (Measure* m = startMeasure; m; m = m->nextMeasureMM()) {
                    mbl.push_back(m);
                    if (m == endMeasure) {
                        mbl.pop_back();
                        break;
                    }
                    if (!toMeasureBase(m)->noBreak()) {
                        allNoBreaks = false;
                    }
                }
            }
        } else {
            // toggle break on the last measure of the range
            mbl.push_back(endMeasure);
            // if more than one measure selected,
            // also toggle break *before* the range (to try to fit selection on a single line)
            if (startMeasure != endMeasure && startMeasure->prev()) {
                mbl.push_back(startMeasure->prev());
            }
        }
    } else {
        MeasureBase* mb = nullptr;
        for (EngravingItem* el : selection().elements()) {
            switch (el->type()) {
            case ElementType::HBOX:
            case ElementType::VBOX:
            case ElementType::TBOX:
                mb = toMeasureBase(el);
                break;
            default: {
                // find measure
                Measure* measure = toMeasure(el->findMeasure());
                // for start repeat, attach break to previous measure
                if (measure && el->isBarLine()) {
                    BarLine* bl = toBarLine(el);
                    if (bl->barLineType() == BarLineType::START_REPEAT) {
                        measure = measure->prevMeasure();
                    }
                }
                // if measure is mmrest, then propagate to last original measure
                if (measure) {
                    mb = measure->isMMRest() ? measure->mmRestLast() : measure;
                }

                if (mb) {
                    allNoBreaks = mb->noBreak();
                }
            }
            }
        }
        if (mb) {
            mbl.push_back(mb);
        }
    }
    // toggle the breaks
    for (MeasureBase* mb: mbl) {
        if (mb) {
            bool val = false;
            switch (type) {
            case LayoutBreakType::LINE:
                val = !mb->lineBreak();
                mb->undoSetBreak(val, type);
                // remove page break if appropriate
                if (val && mb->pageBreak()) {
                    mb->undoSetBreak(false, LayoutBreakType::PAGE);
                }
                break;
            case LayoutBreakType::PAGE:
                val = !mb->pageBreak();
                mb->undoSetBreak(val, type);
                // remove line break if appropriate
                if (val && mb->lineBreak()) {
                    mb->undoSetBreak(false, LayoutBreakType::LINE);
                }
                break;
            case LayoutBreakType::SECTION:
                val = !mb->sectionBreak();
                mb->undoSetBreak(val, type);
                break;
            case LayoutBreakType::NOBREAK:
                mb->undoSetBreak(!allNoBreaks, type);
                // remove other breaks if appropriate
                if (!mb->noBreak()) {
                    if (mb->pageBreak()) {
                        mb->undoSetBreak(false, LayoutBreakType::PAGE);
                    } else if (mb->lineBreak()) {
                        mb->undoSetBreak(false, LayoutBreakType::LINE);
                    } else if (mb->sectionBreak()) {
                        mb->undoSetBreak(false, LayoutBreakType::SECTION);
                    }
                }
                break;
            default:
                break;
            }
        }
    }
}

//---------------------------------------------------------
//   cmdToggleMmrest
//---------------------------------------------------------

void Score::cmdToggleMmrest()
{
    bool val = !styleB(Sid::createMultiMeasureRests);
    deselectAll();
    undo(new ChangeStyleVal(this, Sid::createMultiMeasureRests, val));
}

//---------------------------------------------------------
//   cmdToggleHideEmpty
//---------------------------------------------------------

void Score::cmdToggleHideEmpty()
{
    bool val = !styleB(Sid::hideEmptyStaves);
    deselectAll();
    undo(new ChangeStyleVal(this, Sid::hideEmptyStaves, val));
}

//---------------------------------------------------------
//   cmdSetVisible
//---------------------------------------------------------

void Score::cmdSetVisible()
{
    for (EngravingItem* e : selection().elements()) {
        undo(new ChangeProperty(e, Pid::VISIBLE, true));
    }
}

//---------------------------------------------------------
//   cmdUnsetVisible
//---------------------------------------------------------

void Score::cmdUnsetVisible()
{
    for (EngravingItem* e : selection().elements()) {
        undo(new ChangeProperty(e, Pid::VISIBLE, false));
    }
}

//---------------------------------------------------------
//   cmdAddPitch
///   insert note or add note to chord
//    c d e f g a b entered:
//---------------------------------------------------------

void Score::cmdAddPitch(const EditData& ed, int note, bool addFlag, bool insert)
{
    InputState& is = inputState();
    if (is.track() == mu::nidx) {          // invalid state
        return;
    }
    if (is.segment() == 0) {
        LOGD("cannot enter notes here (no chord rest at current position)");
        return;
    }
    is.setRest(false);
    const Drumset* ds = is.drumset();
    int octave = 4;
    if (ds) {
        char note1 = "CDEFGAB"[note];
        int pitch = -1;
        int voice = 0;
        for (int i = 0; i < 127; ++i) {
            if (!ds->isValid(i)) {
                continue;
            }
            if (ds->shortcut(i) && (ds->shortcut(i) == note1)) {
                pitch = i;
                voice = ds->voice(i);
                break;
            }
        }
        if (pitch == -1) {
            LOGD("  shortcut %c not defined in drumset", note1);
            return;
        }
        is.setDrumNote(pitch);
        is.setTrack(trackZeroVoice(is.track()) + voice);
        octave = pitch / 12;
        if (is.segment()) {
            Segment* seg = is.segment();
            while (seg) {
                if (seg->element(is.track())) {
                    break;
                }
                seg = seg->prev(SegmentType::ChordRest);
            }
            if (seg) {
                is.setSegment(seg);
            } else {
                is.setSegment(is.segment()->measure()->first(SegmentType::ChordRest));
            }
        }
    } else {
        static const int tab[] = { 0, 2, 4, 5, 7, 9, 11 };

        // if adding notes, add above the upNote of the current chord
        EngravingItem* el = selection().element();
        if (addFlag && el && el->isNote()) {
            Chord* chord = toNote(el)->chord();
            Note* n      = chord->upNote();
            int tpc = n->tpc();
            octave = (n->epitch() - int(tpc2alter(tpc))) / PITCH_DELTA_OCTAVE;
            if (note <= tpc2step(tpc)) {
                octave++;
            }
        } else {
            int curPitch = 60;
            if (is.segment()) {
                Staff* staff = Score::staff(is.track() / VOICES);
                Segment* seg = is.segment()->prev1(SegmentType::ChordRest | SegmentType::Clef | SegmentType::HeaderClef);
                while (seg) {
                    if (seg->isChordRestType()) {
                        EngravingItem* p = seg->element(is.track());
                        if (p && p->isChord()) {
                            Note* n = toChord(p)->downNote();
                            // forget any accidental and/or adjustment due to key signature
                            curPitch = n->epitch() - static_cast<int>(tpc2alter(n->tpc()));
                            break;
                        }
                    } else if (seg->isClefType() || seg->isHeaderClefType()) {
                        EngravingItem* p = seg->element(trackZeroVoice(is.track()));              // clef on voice 1
                        if (p && p->isClef()) {
                            Clef* clef = toClef(p);
                            // check if it's an actual change or just a courtesy
                            ClefType ctb = staff->clef(clef->tick() - Fraction::fromTicks(1));
                            if (ctb != clef->clefType() || clef->tick().isZero()) {
                                curPitch = line2pitch(4, clef->clefType(), Key::C);                 // C 72 for treble clef
                                break;
                            }
                        }
                    }
                    seg = seg->prev1MM(SegmentType::ChordRest | SegmentType::Clef | SegmentType::HeaderClef);
                }
                octave = curPitch / 12;
            }

            int delta = octave * 12 + tab[note] - curPitch;
            if (delta > 6) {
                --octave;
            } else if (delta < -6) {
                ++octave;
            }
        }
    }
    ed.view()->startNoteEntryMode();

    int step = octave * 7 + note;
    cmdAddPitch(step,  addFlag, insert);
    ed.view()->adjustCanvasPosition(is.cr());
}

void Score::cmdAddPitch(int step, bool addFlag, bool insert)
{
    insert = insert || inputState().usingNoteEntryMethod(NoteEntryMethod::TIMEWISE);
    Position pos;
    if (addFlag) {
        EngravingItem* el = selection().element();
        if (el && el->isNote()) {
            Note* selectedNote = toNote(el);
            Chord* chord  = selectedNote->chord();
            Segment* seg  = chord->segment();
            pos.segment   = seg;
            pos.staffIdx  = selectedNote->track() / VOICES;
            ClefType clef = staff(pos.staffIdx)->clef(seg->tick());
            pos.line      = relStep(step, clef);
            bool error;
            NoteVal nval = noteValForPosition(pos, _is.accidentalType(), error);
            if (error) {
                return;
            }
            bool forceAccidental = false;
            if (_is.accidentalType() != AccidentalType::NONE) {
                NoteVal nval2 = noteValForPosition(pos, AccidentalType::NONE, error);
                forceAccidental = (nval.pitch == nval2.pitch);
            }
            addNote(chord, nval, forceAccidental, _is.articulationIds());
            _is.setAccidentalType(AccidentalType::NONE);
            return;
        }
    }

    pos.segment   = inputState().segment();
    pos.staffIdx  = inputState().track() / VOICES;
    ClefType clef = staff(pos.staffIdx)->clef(pos.segment->tick());
    pos.line      = relStep(step, clef);

    if (inputState().usingNoteEntryMethod(NoteEntryMethod::REPITCH)) {
        repitchNote(pos, !addFlag);
    } else {
        if (insert) {
            insertChord(pos);
        } else {
            putNote(pos, !addFlag);
        }
    }
    _is.setAccidentalType(AccidentalType::NONE);
}

//---------------------------------------------------------
//   cmdToggleVisible
//---------------------------------------------------------

void Score::cmdToggleVisible()
{
    std::set<EngravingItem*> spanners;
    for (EngravingItem* e : selection().elements()) {
        if (e->isBracket()) {       // ignore
            continue;
        }
        if (e->isNoteDot() && mu::contains(selection().elements(), e->parentItem())) {
            // already handled in ScoreElement::undoChangeProperty(); don't toggle twice
            continue;
        }
        bool spannerSegment = e->isSpannerSegment();
        if (!spannerSegment || !mu::contains(spanners, static_cast<EngravingItem*>(toSpannerSegment(e)->spanner()))) {
            e->undoChangeProperty(Pid::VISIBLE, !e->getProperty(Pid::VISIBLE).toBool());
        }
        if (spannerSegment) {
            spanners.insert(toSpannerSegment(e)->spanner());
        }
    }
}

//---------------------------------------------------------
//   cmdAddFret
///   insert note with given fret on current string
//---------------------------------------------------------

void Score::cmdAddFret(int fret)
{
    InputState& is = inputState();
    if (is.track() == mu::nidx) { // invalid state
        return;
    }
    if (!is.segment()) {
        LOGD("cannot enter notes here (no chord rest at current position)");
        return;
    }
    is.setRest(false);
    Position pos;
    pos.segment   = is.segment();
    pos.staffIdx  = is.track() / VOICES;
    pos.line      = staff(pos.staffIdx)->staffType(is.tick())->physStringToVisual(is.string());
    pos.fret      = fret;
    putNote(pos, false);
}

//---------------------------------------------------------
//   cmdToggleAutoplace
//---------------------------------------------------------

void Score::cmdToggleAutoplace(bool all)
{
    if (all) {
        bool val = !styleB(Sid::autoplaceEnabled);
        undoChangeStyleVal(Sid::autoplaceEnabled, val);
        setLayoutAll();
    } else {
        std::set<EngravingItem*> spanners;
        for (EngravingItem* e : selection().elements()) {
            if (e->isSpannerSegment()) {
                if (EngravingItem* ee = e->propertyDelegate(Pid::AUTOPLACE)) {
                    e = ee;
                }
                // spanner segments may each have their own autoplace setting
                // but if they delegate to spanner, only toggle once
                if (e->isSpanner()) {
                    if (mu::contains(spanners, e)) {
                        continue;
                    }
                    spanners.insert(e);
                }
            }
            PropertyFlags pf = e->propertyFlags(Pid::AUTOPLACE);
            if (pf == PropertyFlags::STYLED) {
                pf = PropertyFlags::UNSTYLED;
            }
            e->undoChangeProperty(Pid::AUTOPLACE, !e->getProperty(Pid::AUTOPLACE).toBool(), pf);
        }
    }
}
}
