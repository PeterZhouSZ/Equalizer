
/* Copyright (c) 2008-2010, Stefan Eilemann <eile@equalizergraphics.com>
 *                          Cedric Stalder <cedric.stalder@gmail.com>
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License version 2.1 as published
 * by the Free Software Foundation.
 *  
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
 * details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "loadEqualizer.h"

#include "../compound.h"
#include "../log.h"

#include <eq/client/statistic.h>
#include <eq/base/debug.h>

namespace eq
{
namespace server
{

std::ostream& operator << ( std::ostream& os, const LoadEqualizer::Node* );

// The tree load balancer organizes the children in a binary tree. At each
// level, a relative split position is determined by balancing the left subtree
// against the right subtree.

LoadEqualizer::LoadEqualizer()
        : _mode( MODE_2D )
        , _damping( .5f )
        , _tree( 0 )
        , _boundary2i( 1, 1 )
        , _boundaryf( std::numeric_limits<float>::epsilon() )

{
    EQINFO << "New LoadEqualizer @" << (void*)this << std::endl;
}

LoadEqualizer::LoadEqualizer( const LoadEqualizer& from )
        : Equalizer( from )
        , ChannelListener( from )
        , _mode( from._mode )
        , _damping( from._damping )
        , _tree( 0 )
        , _boundary2i( from._boundary2i )
        , _boundaryf( from._boundaryf )
{}

LoadEqualizer::~LoadEqualizer()
{
    _clearTree( _tree );
    delete _tree;
    _tree = 0;

    _history.clear();
}

void LoadEqualizer::notifyUpdatePre( Compound* compound,
                                     const uint32_t frameNumber )
{
    if( !_tree )
    {
        EQASSERT( compound == getCompound( ));
        const Compounds& children = compound->getChildren();
        if( children.empty( )) // leaf compound, can't do anything.
            return;

        _tree = _buildTree( children );
        EQLOG( LOG_LB2 ) << "LB tree: " << _tree;
    }

    _checkHistory();

    // execute code above to not leak memory
    if( isFrozen() || !compound->isRunning( ))
        return;

    // compute new data
    _history.push_back( LBFrameData( ));
    _history.back().first = frameNumber;

    _computeSplit();
}

LoadEqualizer::Node* LoadEqualizer::_buildTree( const Compounds& compounds)
{
    Node* node = new Node;

    const size_t size = compounds.size();
    if( size == 1 )
    {
        Compound* compound = compounds.front();

        node->compound  = compound;
        node->splitMode = ( _mode == MODE_2D ) ? MODE_VERTICAL : _mode;

        Channel* channel = compound->getChannel();
        EQASSERT( channel );
        channel->addListener( this );
        return node;
    }

    const size_t middle = size / 2;

    Compounds left;
    for( size_t i = 0; i < middle; ++i )
        left.push_back( compounds[i] );

    Compounds right;
    for( size_t i = middle; i < size; ++i )
        right.push_back( compounds[i] );

    node->left  = _buildTree( left );
    node->right = _buildTree( right );

    if( _mode == MODE_2D )
        node->splitMode = ( node->right->splitMode == MODE_VERTICAL ) ? 
                              MODE_HORIZONTAL : MODE_VERTICAL;
    else
        node->splitMode = _mode;

    node->time      = 0.0f;
    return node;
}

void LoadEqualizer::_clearTree( Node* node )
{
    if( !node )
        return;

    if( node->left )
        _clearTree( node->left );
    if( node->right )
        _clearTree( node->right );

    if( node->compound )
    {
        Channel* channel = node->compound->getChannel();
        EQASSERTINFO( channel, node->compound );
        channel->removeListener( this );
    }
}

void LoadEqualizer::notifyLoadData( Channel* channel,
                                    const uint32_t frameNumber,
                                    const uint32_t nStatistics,
                                    const Statistic* statistics )
{
    for( std::deque< LBFrameData >::iterator i = _history.begin();
         i != _history.end(); ++i )
    {
        LBFrameData& frameData = *i;
        if( frameData.first != frameNumber )
            continue;

        // Found corresponding historical data set
        LBDatas& items = frameData.second;
        for( LBDatas::iterator j = items.begin(); j != items.end(); ++j )
        {
            Data& data = *j;
            if( data.channel != channel )
                continue;

            // Found corresponding historical data item
            const uint32_t taskID = data.taskID;
            EQASSERTINFO( taskID > 0, channel->getName( ));

            if( data.vp.getArea() <= 0.f )
                return;

            // gather relevant load data
            int64_t startTime = std::numeric_limits< int64_t >::max();
            int64_t endTime   = 0;
            bool    loadSet   = false;
            int64_t timeTransmit = 0;
            for( uint32_t k = 0; k < nStatistics && !loadSet; ++k )
            {
                const Statistic& stat = statistics[k];
                if( stat.task != taskID ) // from different compound
                    continue;

                switch( stat.type )
                {
                    case Statistic::CHANNEL_CLEAR:
                    case Statistic::CHANNEL_DRAW:
                    case Statistic::CHANNEL_READBACK:
                        startTime = EQ_MIN( startTime, stat.startTime );
                        endTime   = EQ_MAX( endTime, stat.endTime );
                        break;
                    case Statistic::CHANNEL_FRAME_TRANSMIT:
                        timeTransmit += stat.endTime - stat.startTime;
                        break;
                    // assemble blocks on input frames, stop using subsequent
                    // data
                    case Statistic::CHANNEL_ASSEMBLE:
                        loadSet = true;
                        break;

                    default:
                        break;
                }
            }
    
            if( startTime == std::numeric_limits< int64_t >::max( ))
                return;
    
            data.time = endTime - startTime;
            data.time = EQ_MAX( data.time, 1 );
            data.time = EQ_MAX( data.time, timeTransmit );
            data.load = static_cast< float >( data.time ) / data.vp.getArea();
            EQLOG( LOG_LB2 ) << "Added load "<< data.load << " (t=" << data.time
                            << ") for " << channel->getName() << " " << data.vp
                            << ", " << data.range << " @ " << frameNumber
                            << std::endl;
            return;

            // Note: if the same channel is used twice as a child, the 
            // load-compound association does not work.
        }
    }
}

void LoadEqualizer::_checkHistory()
{
    // 1. Find youngest complete load data set
    uint32_t useFrame = 0;
    for( std::deque< LBFrameData >::reverse_iterator i = _history.rbegin();
         i != _history.rend() && useFrame == 0; ++i )
    {
        const LBFrameData&  frameData  = *i;
        const LBDatas& items      = frameData.second;
        bool                isComplete = true;

        for( LBDatas::const_iterator j = items.begin();
             j != items.end() && isComplete; ++j )
        {
            const Data& data = *j;

            if( data.time < 0 )
                isComplete = false;
        }

        if( isComplete )
            useFrame = frameData.first;
    }

    // delete old, unneeded data sets
    while( !_history.empty() && _history.front().first < useFrame )
        _history.pop_front();
    
    if( _history.empty( )) // insert fake set
    {
        _history.resize( 1 );

        LBFrameData&  frameData  = _history.front();
        LBDatas& items      = frameData.second;

        frameData.first = 0; // frameNumber
        items.resize( 1 );
        
        Data& data = items.front();
        data.time = 1;
        data.load = 1.f;
        EQASSERT( data.taskID == 0 );
        EQASSERT( data.channel == 0 );
    }
}

void LoadEqualizer::_computeSplit()
{
    EQASSERT( !_history.empty( ));
    
    const LBFrameData& frameData = _history.front();
    const Compound* compound = getCompound();
    EQLOG( LOG_LB2 ) << "----- balance " << compound->getChannel()->getName()
                    << " using frame " << frameData.first << std::endl;

    // sort load items for each of the split directions
    LBDatas items( frameData.second );
    _removeEmpty( items );

    LBDatas sortedData[3] = { items, items, items };

    if( _mode == MODE_DB )
    {
        LBDatas& rangeData = sortedData[ MODE_DB ];
        sort( rangeData.begin(), rangeData.end(), _compareRange );
    }
    else
    {
        LBDatas& xData = sortedData[ MODE_VERTICAL ];
        sort( xData.begin(), xData.end(), _compareX );

        LBDatas& yData = sortedData[ MODE_HORIZONTAL ];
        sort( yData.begin(), yData.end(), _compareY );

#ifndef NDEBUG
        for( LBDatas::const_iterator i = xData.begin(); i != xData.end();
             ++i )
        {  
            const Data& data = *i;
            EQLOG( LOG_LB2 ) << "  " << data.vp << ", load " << data.load 
                            << " (t=" << data.load * data.vp.getArea() << ")"
                            << std::endl;
        }
#endif
    }


    // Compute total rendering time
    int64_t totalTime = 0;
    for( LBDatas::const_iterator i = items.begin(); i != items.end(); ++i )
    {  
        const Data& data = *i;
        totalTime += data.time;
    }

    const Compounds& children = compound->getChildren();
    float nResources( 0.f );
    for( Compounds::const_iterator i = children.begin(); 
         i != children.end(); ++i )
    {
        const Compound* child = *i;
        if( child->isRunning( ))
            nResources += child->getUsage();
    }

    const float timeLeft = static_cast< float >( totalTime ) /
                           static_cast< float >( nResources );
    EQLOG( LOG_LB2 ) << "Render time " << totalTime << ", per resource "
                    << timeLeft << ", " << nResources << " resources" << std::endl;

    const float leftover = _assignTargetTimes( _tree, 
                                               static_cast<float>( totalTime ),
                                               timeLeft );
    _assignLeftoverTime( _tree, leftover );
    _computeSplit( _tree, sortedData, Viewport(), Range() );
}

float LoadEqualizer::_assignTargetTimes( Node* node, const float totalTime, 
                                         const float resourceTime )
{
    const Compound* compound = node->compound;
    if( compound )
    {
        const float usage = compound->isRunning() ? compound->getUsage() : 0.f;
        float time = resourceTime * usage;

        if( usage > 0.0f )
        {
            EQASSERT( _damping >= 0.f );
            EQASSERT( _damping <= 1.f );

            const LBFrameData&  frameData = _history.front();
            const LBDatas& items     = frameData.second;
            for( LBDatas::const_iterator i = items.begin(); 
                 i != items.end(); ++i )
            {
                const Data& data = *i;
                const uint32_t taskID = data.taskID;
                
                if( compound->getTaskID() != taskID )
                    continue;

                // found our last rendering time -> use it to smooth the change:
                time = (1.f - _damping) * time + _damping * data.time;
                break;
            }
        }
        const Channel* channel = compound->getChannel();
        node->maxSize.x() = channel->getPixelViewport().w; 
        node->maxSize.y() = channel->getPixelViewport().h; 
        node->boundaryf = _boundaryf;
        node->boundary2i = _boundary2i;
        node->time  = EQ_MIN( time, totalTime );
        node->usage = usage;
        EQLOG( LOG_LB2 ) << compound->getChannel()->getName() << " usage " 
                         << compound->getUsage() << " target " << node->time
                         << ", left " << totalTime - node->time << " max "
                         << node->maxSize << std::endl;

        return totalTime - node->time;
    }

    EQASSERT( node->left );
    EQASSERT( node->right );

    float timeLeft = _assignTargetTimes( node->left, totalTime, resourceTime );
    timeLeft       = _assignTargetTimes( node->right, timeLeft, resourceTime );
    node->time  = node->left->time + node->right->time;
    node->usage = node->left->usage + node->right->usage;
    
    switch( node->splitMode )
    {
        case MODE_VERTICAL:
            node->maxSize.x() = node->left->maxSize.x() +
                                node->right->maxSize.x();  
            node->maxSize.y() = EQ_MIN( node->left->maxSize.y(), 
                                        node->right->maxSize.y() ); 
            node->boundary2i.x() = node->left->boundary2i.x() +
                                   node->right->boundary2i.x();
            node->boundary2i.y() = EQ_MAX( node->left->boundary2i.y(), 
                                           node->right->boundary2i.y() );
            node->boundaryf = EQ_MAX( node->left->boundaryf,
                                      node->right->boundaryf );
            break;
        case MODE_HORIZONTAL:
            node->maxSize.x() = EQ_MIN( node->left->maxSize.x(), 
                                       node->right->maxSize.x() );  
            node->maxSize.y() = node->left->maxSize.y() +
                                node->right->maxSize.y(); 
            node->boundary2i.x() = EQ_MAX( node->left->boundary2i.x(), 
                                           node->right->boundary2i.x() );
            node->boundary2i.y() = node->left->boundary2i.y() +
                                   node->right->boundary2i.y();
            node->boundaryf = EQ_MAX( node->left->boundaryf,
                                      node->right->boundaryf );
            break;
        case MODE_DB:
            node->boundary2i.x() = EQ_MAX( node->left->boundary2i.x(), 
                                           node->right->boundary2i.x() );
            node->boundary2i.y() = EQ_MAX( node->left->boundary2i.y(), 
                                           node->right->boundary2i.y() );
            node->boundaryf = node->left->boundaryf + node->right->boundaryf;
            break;
        default:
            EQUNIMPLEMENTED;
    }

    EQLOG( LOG_LB2 ) << "Node time " << node->time << ", left " << timeLeft
                     << " max " << node->maxSize << std::endl;
    return timeLeft;
}

void LoadEqualizer::_assignLeftoverTime( Node* node, const float time )
{
    const Compound* compound = node->compound;
    if( compound )
    {
        if( node->usage > 0.0f )
            node->time += time;
        else
        {
            EQASSERTINFO( time < 0.0001f, time );
        }
        EQLOG( LOG_LB2 ) << compound->getChannel()->getName() << " usage " 
                        << node->usage << " target " << node->time << std::endl;
    }
    else
    {
        EQASSERT( node->left );
        EQASSERT( node->right );

        if( node->usage > 0.f )
        {
            float leftTime = time * node->left->usage / node->usage;
            float rightTime = time - leftTime;
            if( time - leftTime < 0.0001f )
            {
                leftTime = time;
                rightTime = 0.f;
            }
            else if( time - rightTime < 0.0001f )
            {
                leftTime = 0.f;
                rightTime = time;
            }

            _assignLeftoverTime( node->left, leftTime );
            _assignLeftoverTime( node->right, rightTime );
            node->time = node->left->time + node->right->time;
        }
        else
        {
            EQASSERTINFO( time <= 0.0001f, time );
        }
    }
}

void LoadEqualizer::_removeEmpty( LBDatas& items )
{
    for( LBDatas::iterator i = items.begin(); i != items.end(); )
    {  
        Data& data = *i;

        if( !data.vp.hasArea() || !data.range.hasData( ))
            i = items.erase( i );
        else
            ++i;
    }
}

void LoadEqualizer::_computeSplit( Node* node, LBDatas* sortedData,
                                   const Viewport& vp,
                                   const Range& range )
{
    const float time = node->time;
    EQLOG( LOG_LB2 ) << "_computeSplit " << vp << ", " << range << " time "
                    << time << std::endl;
    EQASSERTINFO( vp.isValid(), vp );
    EQASSERTINFO( range.isValid(), range );
    EQASSERTINFO( node->usage > 0.f || !vp.hasArea() || !range.hasData(),
                  "Assigning work to unused compound: " << vp << ", " << range);

    Compound* compound = node->compound;
    if( compound )
    {
        EQASSERTINFO( vp == Viewport::FULL || range == Range::ALL,
                      "Mixed 2D/DB load-balancing not implemented" );

        // TODO: check that time == vp * load
        compound->setViewport( vp );
        compound->setRange( range );

        EQLOG( LOG_LB2 ) << compound->getChannel()->getName() << " set "
                         << vp << ", " << range << std::endl;

        // save data for later use
        Data data;
        data.vp      = vp;
        data.range   = range;
        data.channel = compound->getChannel();
        data.taskID  = compound->getTaskID();
        EQASSERT( data.taskID > 0 );

        if( !vp.hasArea() || !range.hasData( )) // will not render
            data.time = 0;

        LBFrameData&  frameData = _history.back();
        LBDatas& items     = frameData.second;

        items.push_back( data );
        return;
    }

    EQASSERT( node->left && node->right );

    switch( node->splitMode )
    {
        case MODE_VERTICAL:
        {
            EQASSERT( range == Range::ALL );

            float          timeLeft = node->left->time;
            float          splitPos = vp.x;
            const float    end      = vp.getXEnd();
            LBDatas workingSet = sortedData[ MODE_VERTICAL ];

            while( timeLeft > std::numeric_limits< float >::epsilon() &&
                   splitPos < end && !workingSet.empty())
            {
                EQLOG( LOG_LB2 ) << timeLeft << "ms left for "
                                << workingSet.size() << " tiles" << std::endl;

                // remove all irrelevant items from working set
                //   Is there a more clever way? Erasing invalidates iter, even
                //   if iter is copied and inc'd beforehand.
                LBDatas newSet;
                for( LBDatas::const_iterator i = workingSet.begin();
                     i != workingSet.end(); ++i )
                {
                    const Data& data = *i;
                    if( data.vp.getXEnd() > splitPos )
                        newSet.push_back( data );
                }
                workingSet.swap( newSet );
                EQASSERT( !workingSet.empty( ));

                // find next 'discontinouity' in loads
                float currentPos = 1.0f;
                for( LBDatas::const_iterator i = workingSet.begin();
                     i != workingSet.end(); ++i )
                {
                    const Data& data = *i;                        
                    currentPos = EQ_MIN( currentPos, data.vp.getXEnd( ));
                }

                EQASSERTINFO( currentPos > splitPos,
                              currentPos << "<=" << splitPos );
                EQASSERT( currentPos <= 1.0f );

                // accumulate normalized load in splitPos...currentPos
                EQLOG( LOG_LB2 ) << "Computing load in X " << splitPos << "..."
                                << currentPos << std::endl;
                float currentLoad = 0.f;
                for( LBDatas::const_iterator i = workingSet.begin();
                     i != workingSet.end(); ++i )
                {
                    const Data& data = *i;
                        
                    if( data.vp.x >= currentPos ) // not yet needed data sets
                        break;
#if 0
                    // make sure we cover full area
                    EQASSERTINFO(  data.vp.x <= splitPos, data.vp.x << " > "
                                   << splitPos );
                    EQASSERTINFO( data.vp.getXEnd() >= currentPos, 
                                  data.vp.getXEnd() << " < " << currentPos);
#endif
                    float       yContrib = data.vp.h;

                    if( data.vp.y < vp.y )
                        yContrib -= (vp.y - data.vp.y);
                    
                    const float dataEnd = data.vp.getYEnd();
                    const float vpEnd   = vp.getYEnd();
                    if( dataEnd > vpEnd )
                        yContrib -= (dataEnd - vpEnd);

                    if( yContrib > 0.f )
                    {
                        const float percentage = yContrib / vp.h;
                        EQLOG( LOG_LB2 ) << data.vp << " contributes "
                                        << yContrib << " of " << data.vp.h
                                        << " (" << percentage
                                        << ") with " << data.load << ": "
                                        << ( data.load * percentage )
                                        << " vp.y " << vp.y << " dataEnd " 
                                        << dataEnd << " vpEnd " << vpEnd
                                        << std::endl;

                        currentLoad += ( data.load * percentage );
                    }
                }

                const float width        = currentPos - splitPos;
                const float area         = width * vp.h;
                const float currentTime  = area * currentLoad;
                    
                EQLOG( LOG_LB2 ) << splitPos << "..." << currentPos 
                                << ": t=" << currentTime << " of " 
                                << timeLeft << std::endl;

                if( currentTime >= timeLeft ) // found last region
                {
                    splitPos += (width * timeLeft / currentTime );
                    timeLeft = 0.0f;
                }
                else
                {
                    timeLeft -= currentTime;
                    splitPos  = currentPos;
                }
            }

            EQLOG( LOG_LB2 ) << "Should split at X " << splitPos << std::endl;
            // There might be more time left due to MIN_PIXEL rounding by parent
            // EQASSERTINFO( timeLeft <= .001f, timeLeft );

            // Ensure minimum size
            const Compound* root = getCompound();
            const float pvpW = static_cast< float >(
                root->getInheritPixelViewport().w );
            const float boundary = static_cast< float >( node->boundary2i.x()) /
                                       pvpW;
            if( node->left->usage == 0.f )
                splitPos = vp.x;
            else if( node->right->usage == 0.f )
                splitPos = end;
            else if( boundary > 0 )
            {
                const float lengthRight = vp.getXEnd() - splitPos;
                const float lengthLeft = splitPos - vp.x;
                const float maxRight =
                    static_cast< float >( node->right->maxSize.x( )) / pvpW;
                const float maxLeft =
                    static_cast< float >( node->left->maxSize.x( )) / pvpW;
                if( lengthRight > maxRight )
                    splitPos = end - maxRight;
                else if( lengthLeft > maxLeft )
                    splitPos = vp.x + maxLeft;
            
                if( (splitPos - vp.x) < boundary )
                    splitPos = vp.x + boundary;
                if( (end - splitPos) < boundary )
                    splitPos = end - boundary;
                
                const uint32_t ratio = 
                           static_cast< uint32_t >( splitPos / boundary + .5f );
                splitPos = ratio * boundary;
            }

            splitPos = EQ_MAX( splitPos, vp.x );
            splitPos = EQ_MIN( splitPos, end);

            EQLOG( LOG_LB2 ) << "Split " << vp << " at X " << splitPos
                             << std::endl;

            // balance children
            Viewport childVP = vp;
            childVP.w = (splitPos - vp.x);
            _computeSplit( node->left, sortedData, childVP, range );

            childVP.x = childVP.getXEnd();
            childVP.w = end - childVP.x;
            // Fix 2994111: Rounding errors with 2D LB and 16 sources
            //   Floating point rounding may create a width for the 'right'
            //   child which is slightly below the parent width. Correct it.
            while( childVP.getXEnd() < end )
                childVP.w += std::numeric_limits< float >::epsilon();
            _computeSplit( node->right, sortedData, childVP, range );
            break;
        }

        case MODE_HORIZONTAL:
        {
            EQASSERT( range == Range::ALL );
            float        timeLeft = node->left->time;
            float        splitPos = vp.y;
            const float  end      = vp.getYEnd();
            LBDatas workingSet = sortedData[ MODE_HORIZONTAL ];

            while( timeLeft > std::numeric_limits< float >::epsilon() &&
                   splitPos < end && !workingSet.empty( ))
            {
                EQLOG( LOG_LB2 ) << timeLeft << "ms left for "
                                << workingSet.size() << " tiles" << std::endl;

                // remove all unrelevant items from working set
                //   Is there a more clever way? Erasing invalidates iter, even
                //   if iter is copied and inc'd beforehand.
                LBDatas newSet;
                for( LBDatas::const_iterator i = workingSet.begin();
                     i != workingSet.end(); ++i )
                {
                    const Data& data = *i;
                    if( data.vp.getYEnd() > splitPos )
                        newSet.push_back( data );
                }
                workingSet.swap( newSet );
                EQASSERT( !workingSet.empty( ));

                // find next 'discontinouity' in loads
                float currentPos = 1.0f;
                for( LBDatas::const_iterator i = workingSet.begin();
                     i != workingSet.end(); ++i )
                {
                    const Data& data = *i;                        
                    currentPos = EQ_MIN( currentPos, data.vp.getYEnd( ));
                }

                EQASSERTINFO( currentPos > splitPos,
                              currentPos << "<=" << splitPos );
                EQASSERT( currentPos <= 1.0f );

                // accumulate normalized load in splitPos...currentPos
                EQLOG( LOG_LB2 ) << "Computing load in Y " << splitPos << "..."
                                << currentPos << std::endl;
                float currentLoad = 0.f;
                for( LBDatas::const_iterator i = workingSet.begin();
                     i != workingSet.end(); ++i )
                {
                    const Data& data = *i;
                        
                    if( data.vp.y >= currentPos ) // not yet needed data sets
                        break;

                    float xContrib = data.vp.w;

                    if( data.vp.x < vp.x )
                        xContrib -= (vp.x - data.vp.x);
                    
                    const float dataEnd = data.vp.getXEnd();
                    const float vpEnd   = vp.getXEnd();
                    if( dataEnd > vpEnd )
                        xContrib -= (dataEnd - vpEnd);
                    
                    if( xContrib > 0.f )
                    {
                        const float percentage = xContrib / vp.w;
                        EQLOG( LOG_LB2 ) << data.vp << " contributes "
                                        << xContrib << " of " << data.vp.w
                                        << " (" << percentage
                                        << ") with " << data.load << ": "
                                        << ( data.load * percentage )
                                        << " vp.x " << vp.x << " dataEnd " 
                                        << dataEnd << " vpEnd " << vpEnd
                                        << std::endl;

                        currentLoad += ( data.load * percentage );
                    }
                }

                const float height       = currentPos - splitPos;
                const float area         = height * vp.w;
                const float currentTime  = area * currentLoad;
                    
                EQLOG( LOG_LB2 ) << splitPos << "..." << currentPos 
                                << ": t=" << currentTime << " of " 
                                << timeLeft << std::endl;

                if( currentTime >= timeLeft ) // found last region
                {
                    splitPos += (height * timeLeft / currentTime );
                    timeLeft = 0.0f;
                }
                else
                {
                    timeLeft -= currentTime;
                    splitPos  = currentPos;
                }
            }

            EQLOG( LOG_LB2 ) << "Should split at Y " << splitPos << std::endl;
            // There might be more time left due to MIN_PIXEL rounding by parent
            // EQASSERTINFO( timeLeft <= .001f, timeLeft );

            const Compound* root = getCompound();
            
            const float pvpH = static_cast< float >(
                root->getInheritPixelViewport().h );
            const float boundary = static_cast< float >(node->boundary2i.y( )) /
                                       pvpH;
            
            if( node->left->usage == 0.f )
                splitPos = vp.y;
            else if( node->right->usage == 0.f )
                splitPos = end;
            else if ( boundary > 0 )
            {
                const float lengthRight = vp.getYEnd() - splitPos;
                const float lengthLeft = splitPos - vp.y;
                const float maxRight =
                    static_cast< float >( node->right->maxSize.y( )) / pvpH;
                const float maxLeft =
                    static_cast< float >( node->left->maxSize.y( )) / pvpH;
                if( lengthRight > maxRight )
                    splitPos = end - maxRight;
                else if( lengthLeft > maxLeft )
                    splitPos = vp.y + maxLeft;
                
                if( (splitPos - vp.y) < boundary )
                    splitPos = vp.y + boundary;
                if( (end - splitPos) < boundary )
                    splitPos = end - boundary;
                
                const uint32_t ratio = 
                           static_cast< uint32_t >( splitPos / boundary + .5f );
                splitPos = ratio * boundary;
            }

            splitPos = EQ_MAX( splitPos, vp.y );
            splitPos = EQ_MIN( splitPos, end );

            EQLOG( LOG_LB2 ) << "Split " << vp << " at Y " << splitPos
                             << std::endl;

            Viewport childVP = vp;
            childVP.h = (splitPos - vp.y);
            _computeSplit( node->left, sortedData, childVP, range );

            childVP.y = childVP.getYEnd();
            childVP.h = end - childVP.y;
            while( childVP.getYEnd() < end )
                childVP.h += std::numeric_limits< float >::epsilon();
            _computeSplit( node->right, sortedData, childVP, range );
            break;
        }

        case MODE_DB:
        {
            EQASSERT( vp == Viewport::FULL );
            float          timeLeft = node->left->time;
            float          splitPos = range.start;
            const float    end      = range.end;
            LBDatas workingSet = sortedData[ MODE_DB ];

            while( timeLeft > std::numeric_limits< float >::epsilon() && 
                   splitPos < end && !workingSet.empty( ))
            {
                EQLOG( LOG_LB2 ) << timeLeft << "ms left for "
                                << workingSet.size() << " tiles" << std::endl;

                // remove all irrelevant items from working set
                //   Is there a more clever way? Erasing invalidates iter, even
                //   if iter is copied and inc'd beforehand.
                LBDatas newSet;
                for( LBDatas::const_iterator i = workingSet.begin();
                     i != workingSet.end(); ++i )
                {
                    const Data& data = *i;
                    if( data.range.end > splitPos )
                        newSet.push_back( data );
                }
                workingSet.swap( newSet );
                EQASSERT( !workingSet.empty( ));

                // find next 'discontinouity' in loads
                float currentPos = 1.0f;
                for( LBDatas::const_iterator i = workingSet.begin();
                     i != workingSet.end(); ++i )
                {
                    const Data& data = *i;                        
                    currentPos = EQ_MIN( currentPos, data.range.end );
                }

                EQASSERTINFO( currentPos > splitPos,
                              currentPos << "<=" << splitPos );
                EQASSERT( currentPos <= 1.0f );

                // accumulate normalized load in splitPos...currentPos
                EQLOG( LOG_LB2 ) << "Computing load in range " << splitPos
                                << "..." << currentPos << std::endl;
                float currentLoad = 0.f;
                for( LBDatas::const_iterator i = workingSet.begin();
                     i != workingSet.end(); ++i )
                {
                    const Data& data = *i;
                        
                    if( data.range.start >= currentPos ) // not yet needed data
                        break;
#if 0
                    // make sure we cover full area
                    EQASSERTINFO(  data.range.start <= splitPos, 
                                   data.range.start << " > " << splitPos );
                    EQASSERTINFO( data.range.end >= currentPos, 
                                  data.range.end << " < " << currentPos);
#endif
                    currentLoad += data.load;
                }

                EQLOG( LOG_LB2 ) << splitPos << "..." << currentPos 
                                << ": t=" << currentLoad << " of " 
                                << timeLeft << std::endl;

                if( currentLoad >= timeLeft ) // found last region
                {
                    const float width = currentPos - splitPos;
                    splitPos += (width * timeLeft / currentLoad );
                    timeLeft = 0.0f;
                }
                else
                {
                    timeLeft -= currentLoad;
                    splitPos  = currentPos;
                }
            }
            // There might be more time left due to MIN_PIXEL rounding by parent
            // EQASSERTINFO( timeLeft <= .001f, timeLeft );
            const float boundary( node->boundaryf );
            if( node->left->usage == 0.f )
                splitPos = range.start;
            else if( node->right->usage == 0.f )
                splitPos = end;

            const uint32_t ratio = static_cast< uint32_t >
                      ( splitPos / boundary + .5f );
            splitPos = ratio * boundary;
            if( (splitPos - range.start) < boundary )
                splitPos = range.start;
            if( (end - splitPos) < boundary )
                splitPos = end;

            EQLOG( LOG_LB2 ) << "Split " << range << " at pos " << splitPos
                            << std::endl;

            Range childRange = range;
            childRange.end = splitPos;
            _computeSplit( node->left, sortedData, vp, childRange );

            childRange.start = childRange.end;
            childRange.end   = range.end;
            _computeSplit( node->right, sortedData, vp, childRange );
            break;
        }

        default:
            EQUNIMPLEMENTED;
    }
}

std::ostream& operator << ( std::ostream& os, const LoadEqualizer::Node* node )
{
    if( !node )
        return os;

    os << base::disableFlush;

    if( node->compound )
        os << node->compound->getChannel()->getName() << " target time " 
           << node->time << std::endl;
    else
        os << "split " << node->splitMode << " target time " << node->time
           << std::endl
           << base::indent << node->left << node->right << base::exdent;

    os << base::enableFlush;
    return os;
}

std::ostream& operator << ( std::ostream& os, 
                            const LoadEqualizer::Mode mode )
{
    os << ( mode == LoadEqualizer::MODE_2D         ? "2D" :
            mode == LoadEqualizer::MODE_VERTICAL   ? "VERTICAL" :
            mode == LoadEqualizer::MODE_HORIZONTAL ? "HORIZONTAL" :
            mode == LoadEqualizer::MODE_DB         ? "DB" : "ERROR" );
    return os;
}

std::ostream& operator << ( std::ostream& os, const LoadEqualizer* lb )
{
    if( !lb )
        return os;

    os << base::disableFlush
       << "load_equalizer" << std::endl
       << '{' << std::endl
       << "    mode    " << lb->getMode() << std::endl;
  
    if( lb->getDamping() != 0.5f )
        os << "    damping " << lb->getDamping() << std::endl;

    if( lb->getBoundary2i() != Vector2i( 1, 1 ) )
        os << "    boundary [ " << lb->getBoundary2i().x() << " " 
           << lb->getBoundary2i().y() << " ]" << std::endl;

    if( lb->getBoundaryf() != std::numeric_limits<float>::epsilon() )
        os << "    boundary " << lb->getBoundaryf() << std::endl;

    os << '}' << std::endl << base::enableFlush;
    return os;
}

}
}
