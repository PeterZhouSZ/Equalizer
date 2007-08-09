
/* Copyright (c) 2006-2007, Stefan Eilemann <eile@equalizergraphics.com> 
   All rights reserved. */

#ifndef EQ_PROJECTION_H
#define EQ_PROJECTION_H

#include <eq/base/base.h>
#include <eq/vmmlib/VMMLib.h>

#include <iostream>

namespace eq
{
    /**
     * A projection definition defining a view frustum.
     * 
     * The frustum is defined by a projection system positioned at origin,
     * orientated as defined by the head-pitch-roll angles projecting to a
     * wall at the given distance. The fov defines the horizontal and
     * vertical field of view of the projector.
     */
    class EQ_EXPORT Projection
    {
    public:
        Projection();

        vmml::Vector3f origin;
        float          distance;
        vmml::Vector2f fov;
        vmml::Vector3f hpr;
    };

    EQ_EXPORT std::ostream& operator << ( std::ostream& os, const Projection& );
}

#endif // EQ_PROJECTION_H

