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

#ifndef __SLUR_H__
#define __SLUR_H__

#include "slurtie.h"

#include "global/allocator.h"

namespace mu::engraving {
//---------------------------------------------------------
//   @@ SlurSegment
///    a single segment of slur; also used for Tie
//---------------------------------------------------------

class SlurSegment final : public SlurTieSegment
{
    OBJECT_ALLOCATOR(engraving, SlurSegment)
protected:
    double _extraHeight = 0.0;
    void changeAnchor(EditData&, EngravingItem*) override;
    PointF _endPointOff1 = PointF(0.0, 0.0);
    PointF _endPointOff2 = PointF(0.0, 0.0);

public:
    SlurSegment(System* parent);
    SlurSegment(const SlurSegment& ss);

    SlurSegment* clone() const override { return new SlurSegment(*this); }
    int subtype() const override { return static_cast<int>(spanner()->type()); }
    void draw(mu::draw::Painter*) const override;

    void layoutSegment(const mu::PointF& p1, const mu::PointF& p2);

    bool isEdited() const;
    bool isEndPointsEdited() const;
    bool isEditAllowed(EditData&) const override;
    bool edit(EditData&) override;

    void editDrag(EditData& ed) override;

    Slur* slur() const { return toSlur(spanner()); }
    void adjustEndpoints();
    void computeBezier(mu::PointF so = mu::PointF()) override;
    Shape getSegmentShape(Segment* seg, ChordRest* startCR, ChordRest* endCR);
    void avoidCollisions(PointF& pp1, PointF& p2, PointF& p3, PointF& p4, mu::draw::Transform& toSystemCoordinates, double& slurAngle);
};

//---------------------------------------------------------
//   @@ Slur
//---------------------------------------------------------

class Slur final : public SlurTie
{
    OBJECT_ALLOCATOR(engraving, Slur)

    void slurPosChord(SlurPos*);
    int _sourceStemArrangement = -1;

    friend class Factory;
    Slur(EngravingItem* parent);
    Slur(const Slur&);

public:
    ~Slur() {}

    Slur* clone() const override { return new Slur(*this); }

    void write(XmlWriter& xml) const override;
    bool readProperties(XmlReader&) override;
    void layout() override;
    SpannerSegment* layoutSystem(System*) override;
    void setTrack(track_idx_t val) override;
    void slurPos(SlurPos*) override;
    void fixArticulations(PointF& pt, Chord* c, double up, bool stemSide);
    void computeUp();

    SlurSegment* frontSegment() { return toSlurSegment(Spanner::frontSegment()); }
    const SlurSegment* frontSegment() const { return toSlurSegment(Spanner::frontSegment()); }
    SlurSegment* backSegment() { return toSlurSegment(Spanner::backSegment()); }
    const SlurSegment* backSegment() const { return toSlurSegment(Spanner::backSegment()); }
    SlurSegment* segmentAt(int n) { return toSlurSegment(Spanner::segmentAt(n)); }
    const SlurSegment* segmentAt(int n) const { return toSlurSegment(Spanner::segmentAt(n)); }

    bool isCrossStaff();
    bool isOverBeams();

    SlurTieSegment* newSlurTieSegment(System* parent) override { return new SlurSegment(parent); }
};
} // namespace mu::engraving
#endif
