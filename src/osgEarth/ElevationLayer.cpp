/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
 * Copyright 2016 Pelican Mapping
 * http://osgearth.org
 *
 * osgEarth is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */
#include <osgEarth/ElevationLayer>
#include <osgEarth/VerticalDatum>
#include <osgEarth/HeightFieldUtils>
#include <osgEarth/Progress>
#include <osgEarth/MemCache>
#include <osgEarth/Metrics>
#include <osgEarth/ImageUtils>
#include <osg/Version>
#include <iterator>

using namespace osgEarth;
using namespace OpenThreads;

#define LC "[ElevationLayer] \"" << getName() << "\" : "

namespace osgEarth {
    REGISTER_OSGEARTH_LAYER(elevation, osgEarth::ElevationLayer);
}

//------------------------------------------------------------------------

ElevationLayerOptions::ElevationLayerOptions() :
TerrainLayerOptions()
{
    setDefaults();
    fromConfig(_conf);
}

ElevationLayerOptions::ElevationLayerOptions(const ConfigOptions& options) :
TerrainLayerOptions( options )
{
    setDefaults();
    fromConfig( _conf );
}

ElevationLayerOptions::ElevationLayerOptions(const std::string& name) :
TerrainLayerOptions( name )
{
    setDefaults();
    fromConfig( _conf );
}

ElevationLayerOptions::ElevationLayerOptions(const std::string& name, const TileSourceOptions& driverOptions) :
TerrainLayerOptions( name, driverOptions )
{
    setDefaults();
    fromConfig( _conf );
}

void
ElevationLayerOptions::setDefaults()
{
    _offset.init( false );
    _noDataPolicy.init( NODATA_INTERPOLATE );
}

Config
ElevationLayerOptions::getConfig() const
{
    Config conf = TerrainLayerOptions::getConfig();
    conf.key() = "elevation";

    conf.set("offset", _offset);
    conf.set("nodata_policy", "default",     _noDataPolicy, NODATA_INTERPOLATE );
    conf.set("nodata_policy", "interpolate", _noDataPolicy, NODATA_INTERPOLATE );
    conf.set("nodata_policy", "msl",         _noDataPolicy, NODATA_MSL );

    //if (driver().isSet())
    //    conf.set("driver", driver()->getDriver());

    return conf;
}

void
ElevationLayerOptions::fromConfig( const Config& conf )
{
    conf.getIfSet("offset", _offset );
    conf.getIfSet("nodata_policy", "default",     _noDataPolicy, NODATA_INTERPOLATE );
    conf.getIfSet("nodata_policy", "interpolate", _noDataPolicy, NODATA_INTERPOLATE );
    conf.getIfSet("nodata_policy", "msl",         _noDataPolicy, NODATA_MSL );
}

void
ElevationLayerOptions::mergeConfig( const Config& conf )
{
    TerrainLayerOptions::mergeConfig( conf );
    fromConfig( conf );
}

//------------------------------------------------------------------------

namespace
{
    // Opeartion that replaces invalid heights with the NO_DATA_VALUE marker.
    struct NormalizeNoDataValues : public TileSource::HeightFieldOperation
    {
        NormalizeNoDataValues(TerrainLayer* layer)
        {
            _noDataValue   = layer->getNoDataValue();
            _minValidValue = layer->getMinValidValue();
            _maxValidValue = layer->getMaxValidValue();
        }

        void operator()(osg::ref_ptr<osg::HeightField>& hf)
        {
            if ( hf.valid() )
            {
                osg::FloatArray* values = hf->getFloatArray();
                for(osg::FloatArray::iterator i = values->begin(); i != values->end(); ++i)
                {
                    float& value = *i;
                    if ( osg::isNaN(value) || osg::equivalent(value, _noDataValue) || value < _minValidValue || value > _maxValidValue )
                    {
                        OE_DEBUG << "Replaced " << value << " with NO_DATA_VALUE" << std::endl;
                        value = NO_DATA_VALUE;
                    }
                } 
            }
        }

        float _noDataValue, _minValidValue, _maxValidValue;
    };

    // perform very basic sanity-check validation on a heightfield.
    bool validateHeightField(osg::HeightField* hf)
    {
        if (!hf) 
            return false;
        if (hf->getNumRows() < 2 || hf->getNumRows() > 1024)
            return false;
        if (hf->getNumColumns() < 2 || hf->getNumColumns() > 1024)
            return false;
        if (hf->getHeightList().size() != hf->getNumColumns() * hf->getNumRows())
            return false;
        //if (hf->getXInterval() < 1e-5 || hf->getYInterval() < 1e-5)
        //    return false;
        
        return true;
    }    
}

//------------------------------------------------------------------------

ElevationLayer::ElevationLayer() :
TerrainLayer(&_optionsConcrete),
_options(&_optionsConcrete)
{
    init();
}

ElevationLayer::ElevationLayer(const ElevationLayerOptions& options) :
TerrainLayer(&_optionsConcrete),
_options(&_optionsConcrete),
_optionsConcrete(options)
{
    init();
}

ElevationLayer::ElevationLayer(const std::string& name, const TileSourceOptions& driverOptions) :
TerrainLayer(&_optionsConcrete),
_options(&_optionsConcrete),
_optionsConcrete(name, driverOptions)
{
    init();
}

ElevationLayer::ElevationLayer(const ElevationLayerOptions& options, TileSource* tileSource) :
TerrainLayer(&_optionsConcrete, tileSource),
_options(&_optionsConcrete),
_optionsConcrete(options)
{
    init();
}

ElevationLayer::ElevationLayer(ElevationLayerOptions* optionsPtr) :
TerrainLayer(optionsPtr? optionsPtr : &_optionsConcrete),
_options(optionsPtr? optionsPtr : &_optionsConcrete)
{
    //init(); // will be called by subclass.
}

void
ElevationLayer::init()
{
    TerrainLayer::init();

    // elevation layers do not render directly; rather, a composite of elevation data
    // feeds the terrain engine to permute the mesh.
    setRenderType(RENDERTYPE_NONE);
}

bool
ElevationLayer::isOffset() const
{
    return options().offset().get();
}

TileSource::HeightFieldOperation*
ElevationLayer::getOrCreatePreCacheOp()
{
    if ( !_preCacheOp.valid() )
    {
        Threading::ScopedMutexLock lock(_mutex);
        if ( !_preCacheOp.valid() )
        {
            _preCacheOp = new NormalizeNoDataValues(this);
        }
    }
    return _preCacheOp.get();
}

void
ElevationLayer::createImplementation(const TileKey& key,
                                     osg::ref_ptr<osg::HeightField>& out_hf,
                                     osg::ref_ptr<NormalMap>& out_normalMap,
                                     ProgressCallback* progress)
{
    out_hf = createHeightFieldFromTileSource(key, progress);
    
    // Do not create a normal map here. The populateHeightField method will
    // create the normal map.
}

osg::HeightField*
ElevationLayer::createHeightFieldFromTileSource(const TileKey&    key,
                                                ProgressCallback* progress)
{
    osg::ref_ptr<osg::HeightField> result;

    TileSource* source = getTileSource();
    if ( !source )
        return 0L;

    // If the key is blacklisted, fail.
    if ( source->getBlacklist()->contains( key ))
    {
        OE_DEBUG << LC << "Tile " << key.str() << " is blacklisted " << std::endl;
        return 0L;
    }

    // If the profiles are horizontally equivalent (different vdatums is OK), take the
    // quick route:
    if ( key.getProfile()->isHorizEquivalentTo( getProfile() ) )
    {
        // Only try to get data if the source actually has data
        if (!mayHaveData(key))
        {
            OE_DEBUG << LC << "Source for layer has no data at " << key.str() << std::endl;
            return 0L;
        }

        // Make it from the source:
        result = source->createHeightField( key, getOrCreatePreCacheOp(), progress );
   
        // If the result is good, we how have a heightfield but it's vertical values
        // are still relative to the tile source's vertical datum. Convert them.
        if (result.valid())
        {
            if ( ! key.getExtent().getSRS()->isVertEquivalentTo( getProfile()->getSRS() ) )
            {
                VerticalDatum::transform(
                    getProfile()->getSRS()->getVerticalDatum(),    // from
                    key.getExtent().getSRS()->getVerticalDatum(),  // to
                    key.getExtent(),
                    result );
            }
        }
        
        // Blacklist the tile if it is the same projection as the source and
        // we can't get it and it wasn't cancelled
        if (!result.valid())
        {
            if ( progress == 0L ||
                 ( !progress->isCanceled() && !progress->needsRetry() ) )
            {
                source->getBlacklist()->add( key );
            }
        }
    }

    // Otherwise, profiles don't match so we need to composite:
    else
    {
        // note: this method takes care of the vertical datum shift internally.
        osg::ref_ptr<NormalMap> dummyNormalMap;
        assembleHeightField( key, result, dummyNormalMap, progress );
    }

    return result.release();
}


void
ElevationLayer::assembleHeightField(const TileKey& key,
                                    osg::ref_ptr<osg::HeightField>& out_hf,
                                    osg::ref_ptr<NormalMap>& out_normalMap,
                                    ProgressCallback* progress)
{			
    //osg::HeightField* result = 0L;

    // Collect the heightfields for each of the intersecting tiles.
    GeoHeightFieldVector heightFields;

    //Determine the intersecting keys
    std::vector< TileKey > intersectingTiles;
    getProfile()->getIntersectingTiles( key, intersectingTiles );

    // collect heightfield for each intersecting key. Note, we're hitting the
    // underlying tile source here, so there's no vetical datum shifts happening yet.
    // we will do that later.
    if ( intersectingTiles.size() > 0 )
    {
        for (unsigned int i = 0; i < intersectingTiles.size(); ++i)
        {
            const TileKey& layerKey = intersectingTiles[i];

            if ( isKeyInLegalRange(layerKey) )
            {
                osg::ref_ptr<osg::HeightField> hf;
                osg::ref_ptr<NormalMap> normalMap;
                createImplementation(layerKey, hf, normalMap, progress);
                //osg::HeightField* hf = createHeightFieldImplementation( layerKey, progress );
                if (hf.valid())
                {
                    heightFields.push_back( GeoHeightField(hf.get(), normalMap.get(), layerKey.getExtent()) );
                }
            }
        }
    }

    // If we actually got a HeightField, resample/reproject it to match the incoming TileKey's extents.
    if (heightFields.size() > 0)
    {		
        unsigned int width = 0;
        unsigned int height = 0;

        for (GeoHeightFieldVector::iterator itr = heightFields.begin(); itr != heightFields.end(); ++itr)
        {
            if (itr->getHeightField()->getNumColumns() > width)
                width = itr->getHeightField()->getNumColumns();
            if (itr->getHeightField()->getNumRows() > height) 
                height = itr->getHeightField()->getNumRows();                        
        }

        //Now sort the heightfields by resolution to make sure we're sampling the highest resolution one first.
        std::sort( heightFields.begin(), heightFields.end(), GeoHeightField::SortByResolutionFunctor());        

        out_hf = new osg::HeightField();
        out_hf->allocate(width, height);

        out_normalMap = new NormalMap(width, height);

        //Go ahead and set up the heightfield so we don't have to worry about it later
        double minx, miny, maxx, maxy;
        key.getExtent().getBounds(minx, miny, maxx, maxy);
        double dx = (maxx - minx)/(double)(width-1);
        double dy = (maxy - miny)/(double)(height-1);

        //Create the new heightfield by sampling all of them.
        for (unsigned int c = 0; c < width; ++c)
        {
            double x = minx + (dx * (double)c);
            for (unsigned r = 0; r < height; ++r)
            {
                double y = miny + (dy * (double)r);

                //For each sample point, try each heightfield.  The first one with a valid elevation wins.
                float elevation = NO_DATA_VALUE;
                osg::Vec3 normal(0,0,1);

                for (GeoHeightFieldVector::iterator itr = heightFields.begin(); itr != heightFields.end(); ++itr)
                {
                    // get the elevation value, at the same time transforming it vertically into the 
                    // requesting key's vertical datum.
                    float e = 0.0;
                    osg::Vec3 n;
                    if (itr->getElevationAndNormal(key.getExtent().getSRS(), x, y, INTERP_BILINEAR, key.getExtent().getSRS(), e, n))
                    {
                        elevation = e;
                        normal = n;
                        break;
                    }
                }
                out_hf->setHeight( c, r, elevation );   
                out_normalMap->set( c, r, normal );
            }
        }
    }
}

GeoHeightField
ElevationLayer::createHeightField(const TileKey& key)
{
    return createHeightField(key, 0L);
}

GeoHeightField
ElevationLayer::createHeightField(const TileKey&    key,
                                  ProgressCallback* progress )
{
    METRIC_SCOPED_EX("ElevationLayer::createHeightField", 2,
                     "key", key.str().c_str(),
                     "name", getName().c_str());

    if (getStatus().isError())
    {
        return GeoHeightField::INVALID;
    }

    // If the layer is disabled, bail out.
    if ( getEnabled() == false )
    {
        return GeoHeightField::INVALID;
    }

    GeoHeightField result;
    osg::ref_ptr<osg::HeightField> hf;
    osg::ref_ptr<NormalMap> normalMap;

    // Check the memory cache first
    bool fromMemCache = false;

    // cache key combines the key with the full signature (incl vdatum)
    std::string cacheKey = Stringify() << key.str() << "_" << key.getProfile()->getFullSignature();
    const CachePolicy& policy = getCacheSettings()->cachePolicy().get();

    if ( _memCache.valid() )
    {
        CacheBin* bin = _memCache->getOrCreateDefaultBin();
        ReadResult cacheResult = bin->readObject(cacheKey, 0L);
        if ( cacheResult.succeeded() )
        {
            result = GeoHeightField(
                static_cast<osg::HeightField*>(cacheResult.releaseObject()),
                key.getExtent());

            fromMemCache = true;
        }
    }

    if ( !result.valid() )
    {
        // See if there's a persistent cache.
        CacheBin* cacheBin = getCacheBin( key.getProfile() );

        // Can we continue? Only if either:
        //  a) there is a valid tile source plugin;
        //  b) a tile source is not expected, meaning the subclass overrides getHeightField; or
        //  c) we are in cache-only mode and there is a valid cache bin.
        bool canContinue =
            getTileSource() ||
            !isTileSourceExpected() ||
            (policy.isCacheOnly() && cacheBin != 0L);

        if (!canContinue)
        {
            disable("Error: layer does not have a valid TileSource, cannot create heightfield");
            return GeoHeightField::INVALID;
        }

        // validate the existance of a valid layer profile.
        if ( !policy.isCacheOnly() && !getProfile() )
        {
            disable("Could not establish a valid profile.. did you set one?");
            return GeoHeightField::INVALID;
        }

        // Now attempt to read from the cache. Since the cached data is stored in the
        // map profile, we can try this first.
        bool fromCache = false;

        osg::ref_ptr< osg::HeightField > cachedHF;

        if ( cacheBin && policy.isCacheReadable() )
        {
            ReadResult r = cacheBin->readObject(cacheKey, 0L);
            if ( r.succeeded() )
            {            
                bool expired = policy.isExpired(r.lastModifiedTime());
                cachedHF = r.get<osg::HeightField>();
                if ( cachedHF && validateHeightField(cachedHF) )
                {
                    if (!expired)
                    {
                        hf = cachedHF;
                        fromCache = true;
                    }
                }
            }
        }

        // if we're cache-only, but didn't get data from the cache, fail silently.
        if ( !hf.valid() && policy.isCacheOnly() )
        {
            return GeoHeightField::INVALID;
        }

        if ( !hf.valid() )
        {
            if ( !isKeyInLegalRange(key) )
                return GeoHeightField::INVALID;

            // If no tile source is expected, create a height field by calling
            // the raw inheritable method.
            if (!isTileSourceExpected())
            {
                createImplementation(key, hf, normalMap, progress);
                //hf = createHeightFieldImplementation(key, progress);
            }

            else
            {
                // bad tilesource? fail
                if ( !getTileSource() || !getTileSource()->isOK() )
                    return GeoHeightField::INVALID;

                // build a HF from the TileSource.
                //hf = createHeightFieldImplementation( key, progress );
                createImplementation(key, hf, normalMap, progress);
            }

            // validate it to make sure it's legal.
            if ( hf.valid() && !validateHeightField(hf.get()) )
            {
                OE_WARN << LC << "Driver " << getTileSource()->getName() << " returned an illegal heightfield" << std::endl;
                hf = 0L; // to fall back on cached data if possible.
            }

            // cache if necessary
            if ( hf            && 
                 cacheBin      && 
                 !fromCache    &&
                 policy.isCacheWriteable() )
            {
                cacheBin->write(cacheKey, hf, 0L);
            }

            // We have an expired heightfield from the cache and no new data from the TileSource.  So just return the cached data.
            if (!hf.valid() && cachedHF.valid())
            {
                OE_DEBUG << LC << "Using cached but expired heightfield for " << key.str() << std::endl;
                hf = cachedHF;
            }

            if ( !hf.valid() )
            {
                return GeoHeightField::INVALID;
            }

            // Set up the heightfield params.
            double minx, miny, maxx, maxy;
            key.getExtent().getBounds(minx, miny, maxx, maxy);
            hf->setOrigin( osg::Vec3d( minx, miny, 0.0 ) );
            double dx = (maxx - minx)/(double)(hf->getNumColumns()-1);
            double dy = (maxy - miny)/(double)(hf->getNumRows()-1);
            hf->setXInterval( dx );
            hf->setYInterval( dy );
            hf->setBorderWidth( 0 );
        }

        if ( hf.valid() )
        {
            result = GeoHeightField( hf.get(), normalMap.get(), key.getExtent() );
        }
    }

    // write to mem cache if needed:
    if ( result.valid() && !fromMemCache && _memCache.valid() )
    {
        CacheBin* bin = _memCache->getOrCreateDefaultBin();
        bin->write(cacheKey, result.getHeightField(), 0L);
    }

    // post-processing:
    if ( result.valid() )
    {
        if ( options().noDataPolicy() == NODATA_MSL )
        {
            // requested VDatum:
            const VerticalDatum* outputVDatum = key.getExtent().getSRS()->getVerticalDatum();
            const Geoid* geoid = 0L;

            // if there's an output vdatum, just set all invalid's to zero MSL.
            if ( outputVDatum == 0L )
            {
                // if the output is geodetic (HAE), but the input has a geoid, 
                // use that geoid to populate the invalid data at sea level.
                const VerticalDatum* profileDatum  = getProfile()->getSRS()->getVerticalDatum();
                if ( profileDatum )
                    geoid = profileDatum->getGeoid();
            }

            HeightFieldUtils::resolveInvalidHeights(
                result.getHeightField(),
                result.getExtent(),
                NO_DATA_VALUE,
                geoid );
        }
    }

    return result;
}


//------------------------------------------------------------------------

#undef  LC
#define LC "[ElevationLayers] "

ElevationLayerVector::ElevationLayerVector()
{
    //nop
}


ElevationLayerVector::ElevationLayerVector(const ElevationLayerVector& rhs) :
osg::MixinVector< osg::ref_ptr<ElevationLayer> >( rhs )
{
    //nop
}



namespace
{
    typedef osg::ref_ptr<ElevationLayer>          RefElevationLayer;
    struct LayerData {
        RefElevationLayer layer;
        TileKey key;
        int index;
    };
    //typedef std::pair<RefElevationLayer, TileKey> LayerAndKey;
    typedef std::vector<LayerData>              LayerDataVector;

    //! Gets the normal vector for elevation data at column s, row t.
    osg::Vec3 getNormal(const GeoExtent& extent, const osg::HeightField* hf, int s, int t)
    {
        int w = hf->getNumColumns();
        int h = hf->getNumRows();

        osg::Vec2d res(
            extent.width() / (double)(w-1),
            extent.height() / (double)(h-1));

        float e = hf->getHeight(s, t);

        double dx = res.x(), dy = res.y();

        if (extent.getSRS()->isGeographic())
        {
            double R = extent.getSRS()->getEllipsoid()->getRadiusEquator();
            double mPerDegAtEquator = (2.0 * osg::PI * R) / 360.0;
            dy = dy * mPerDegAtEquator;
            double lat = extent.yMin() + res.y()*(double)t;
            dx = dx * mPerDegAtEquator * cos(osg::DegreesToRadians(lat));
        }
        
        osg::Vec3d west(0, 0, e), east(0, 0, e), south(0, 0, e), north(0, 0, e);

        if (s > 0)     west.set (-dx, 0, hf->getHeight(s-1, t));
        if (s < w - 1) east.set ( dx, 0, hf->getHeight(s+1, t));
        if (t > 0)     south.set(0, -dy, hf->getHeight(s, t-1));
        if (t < h - 1) north.set(0,  dy, hf->getHeight(s, t+1));

        osg::Vec3d normal = (east - west) ^ (north - south);
        return normal;
    }

    //! Creates a normal map for heightfield "hf" and stores it in the
    //! pre-allocated NormalMap.
    //!
    //! "deltaLOD" holds the difference in LODs between the heightfield itself and the LOD
    //! from which the elevation value came. This will be positive when we had to "fall back" on 
    //! lower LOD data to fetch an elevation value. When this happens we need to interpolate
    //! between "real" normals instead of sampling them from the neighboring pixels, otherwise
    //! ugly faceting will occur.
    //!
    //! Unfortunately, it there's an offset layer, this will update the deltaLOD and we will 
    //! get faceting if the real elevation layer is from a lower LOD. The only solution to this
    //! would be to sample the elevation data using a spline function instead of bilinear
    //! interpolation -- but we would need to do that to a separate heightfield (especially for
    //! normals) in order to maintain terrain correlation. Maybe someday.
    void createNormalMap(const GeoExtent& extent, const osg::HeightField* hf, const osg::ShortArray* deltaLOD, NormalMap* normalMap)
    {
        int w = hf->getNumColumns();
        int h = hf->getNumRows();

        for (int t = 0; t < hf->getNumRows(); ++t)
        {
            for (int s = 0; s<hf->getNumColumns(); ++s)
            {
                int step = 1 << (*deltaLOD)[t*h + s];

                osg::Vec3 normal;

                // four corners:
                int s0=s, s1=s, t0=t, t1=t;

                if (step == 1)
                {
                    // Same LOD, simple query
                    normal = getNormal(extent, hf, s, t);
                }
                else
                {
                    int s0 = std::max(s - (s % step), 0);
                    int s1 = (s%step == 0)? s0 : std::min(s0+step, w-1);
                    int t0 = std::max(t - (t % step), 0);
                    int t1 = (t%step == 0)? t0 : std::min(t0+step, h-1);
                    
                    if (s0 == s1 && t0 == t1)
                    {
                        // on-pixel, simple query
                        normal = getNormal(extent, hf, s0, t0);
                    }
                    else if (s0 == s1)
                    {
                        // same column; linear interpolate along row
                        osg::Vec3 S = getNormal(extent, hf, s0, t0);
                        osg::Vec3 N = getNormal(extent, hf, s0, t1);
                        normal = S*(double)(t1 - t) + N*(double)(t - t0);
                    }
                    else if (t0 == t1)
                    {
                        // same row; linear interpolate along column
                        osg::Vec3 W = getNormal(extent, hf, s0, t0);
                        osg::Vec3 E = getNormal(extent, hf, s1, t0);
                        normal = W*(double)(s1 - s) + E*(double)(s - s0);
                    }
                    else
                    {
                        // bilinear interpolate
                        osg::Vec3 SW = getNormal(extent, hf, s0, t0);
                        osg::Vec3 SE = getNormal(extent, hf, s1, t0);
                        osg::Vec3 NW = getNormal(extent, hf, s0, t1);
                        osg::Vec3 NE = getNormal(extent, hf, s1, t1);

                        osg::Vec3 S = SW*(double)(s1 - s) + SE*(double)(s - s0);
                        osg::Vec3 N = NW*(double)(s1 - s) + NE*(double)(s - s0);
                        normal = S*(double)(t1 - t) + N*(double)(t - t0);
                    }
                }

                normal.normalize();

                normalMap->set(s, t, normal, 0.0f);
            }
        }
    }
}

bool
ElevationLayerVector::populateHeightFieldAndNormalMap(osg::HeightField*      hf,
                                                      NormalMap*             normalMap,
                                                      const TileKey&         key,
                                                      const Profile*         haeProfile,
                                                      ElevationInterpolation interpolation,
                                                      ProgressCallback*      progress ) const
{
    // heightfield must already exist.
    if ( !hf )
        return false;

    METRIC_SCOPED("ElevationLayer.populateHeightField");

    // if the caller provided an "HAE map profile", he wants an HAE elevation grid even if
    // the map profile has a vertical datum. This is the usual case when building the 3D
    // terrain, for example. Construct a temporary key that doesn't have the vertical
    // datum info and use that to query the elevation data.
    TileKey keyToUse = key;
    if ( haeProfile )
    {
        keyToUse = TileKey(key.getLOD(), key.getTileX(), key.getTileY(), haeProfile );
    }
    
    // Collect the valid layers for this tile.
    LayerDataVector contenders;
    LayerDataVector offsets;

    // Track the number of layers that would return fallback data.
    unsigned numFallbackLayers = 0;

    // Check them in reverse order since the highest priority is last.
    for (int i = size()-1; i>=0; --i)
    //for(ElevationLayerVector::const_reverse_iterator i = this->rbegin(); i != this->rend(); ++i)
    {
        ElevationLayer* layer = (*this)[i].get(); //i->get();

        if ( layer->getEnabled() && layer->getVisible() )
        {
            // calculate the resolution-mapped key (adjusted for tile resolution differential).            
            TileKey mappedKey = keyToUse.mapResolution(
                hf->getNumColumns(),
                layer->getTileSize() );

            bool useLayer = true;
            TileKey bestKey( mappedKey );

            // Check whether the non-mapped key is valid according to the user's min/max level settings:
            if ( !layer->isKeyInLegalRange(key) )
            {
                useLayer = false;
            }
                
            // Find the "best available" mapped key from the tile source:
            else 
            {
                bestKey = layer->getBestAvailableTileKey(mappedKey);
                if (bestKey.valid())
                {
                    // If the bestKey is not the mappedKey, this layer is providing
                    // fallback data (data at a lower resolution than requested)
                    if ( mappedKey != bestKey )
                    {
                        numFallbackLayers++;
                    }
                }
                else
                {
                    useLayer = false;
                }
            }

            if ( useLayer )
            {
                if ( layer->isOffset() )
                {
                    offsets.push_back(LayerData());
                    LayerData& ld = offsets.back();
                    ld.layer = layer;
                    ld.key = bestKey;
                    ld.index = i;
                }
                else
                {
                    contenders.push_back(LayerData());
                    LayerData& ld = contenders.back();
                    ld.layer = layer;
                    ld.key = bestKey;
                    ld.index = i;
                }
            }
        }
    }

    // nothing? bail out.
    if ( contenders.empty() && offsets.empty() )
    {
        return false;
    }

    // if everything is fallback data, bail out.
    if ( contenders.size() + offsets.size() == numFallbackLayers )
    {
        return false;
    }
    
    // Sample the layers into our target.
    unsigned numColumns = hf->getNumColumns();
    unsigned numRows    = hf->getNumRows();    
    double   xmin       = key.getExtent().xMin();
    double   ymin       = key.getExtent().yMin();
    double   dx         = key.getExtent().width() / (double)(numColumns-1);
    double   dy         = key.getExtent().height() / (double)(numRows-1);

#if 0
    // If the incoming heightfield requests a positive border width, 
    // we need to adjust the extents so that we request data outside the
    // extent of the tile key:
    unsigned border = hf->getBorderWidth();
    if (border > 0u)
    {
        dx = key.getExtent().width() / (double)(numColumns - (border*2+1));
        dy = key.getExtent().height() / (double)(numRows - (border*2+1));
        xmin -= dx * (double)border;
        ymin -= dy * (double)border;
    }
#endif
    
    // We will load the actual heightfields on demand. We might not need them all.
#if 0
    GeoHeightFieldVector heightFields(contenders.size());
    GeoHeightFieldVector offsetFields(offsets.size());
    std::vector<bool>    heightFallback(contenders.size(), false);
    std::vector<bool>    heightFailed(contenders.size(), false);
    std::vector<bool>    offsetFailed(offsets.size(), false);
#else
    GeoHeightFieldVector heightFields[9];
    GeoHeightFieldVector offsetFields[9]; //(offsets.size());
    std::vector<bool>    heightFallback[9]; //(contenders.size(), false);
    std::vector<bool>    heightFailed[9]; //(contenders.size(), false);
    std::vector<bool>    offsetFailed[9]; //(offsets.size(), false);

    for (int n = 0; n < 9; ++n)
    {
        heightFields[n].resize(contenders.size());
        offsetFields[n].resize(offsets.size());
        heightFallback[n].assign(9, false);
        heightFailed[n].assign(9, false);
        offsetFailed[n].assign(9, false);
    }
#endif

    // The maximum number of heightfields to keep in this local cache
    unsigned int maxHeightFields = 50;
    unsigned numHeightFieldsInCache = 0;

    const SpatialReference* keySRS = keyToUse.getProfile()->getSRS();

    bool realData = false;

    unsigned int total = numColumns * numRows;

    // query resolution interval (x, y) of each sample.
    osg::ref_ptr<osg::ShortArray> deltaLOD = new osg::ShortArray(total);
    
    int nodataCount = 0;

    for (unsigned c = 0; c < numColumns; ++c)
    {
        double x = xmin + (dx * (double)c);
        for (unsigned r = 0; r < numRows; ++r)
        {
            double y = ymin + (dy * (double)r);

            // Collect elevations from each layer as necessary.
            int resolvedIndex = -1;

            osg::Vec3 normal_sum(0,0,0);
            unsigned normal_count = 0u;

            for(int i=0; i<contenders.size() && resolvedIndex<0; ++i)
            {
                ElevationLayer* layer = contenders[i].layer.get();                
                TileKey& contenderKey = contenders[i].key;
                int index = contenders[i].index;

                // If there is a border, the edge points may not fall within the key extents 
                // and we may need to fetch a neighboring key.

                int n = 4; // index 4 is the center/default tile

#if 0
                if (border > 0u && !contenderKey.getExtent().contains(x, y))
                {
                    int dTx = x < contenderKey.getExtent().xMin() ? -1 : x > contenderKey.getExtent().xMax() ? +1 : 0;
                    int dTy = y < contenderKey.getExtent().yMin() ? +1 : y > contenderKey.getExtent().yMax() ? -1 : 0;
                    contenderKey = contenderKey.createNeighborKey(dTx, dTy);
                    n = (dTy+1)*3 + (dTx+1);
                }
#endif

                if ( heightFailed[n][i] )
                    continue;

                TileKey& actualKey = contenderKey;

                GeoHeightField& layerHF = heightFields[n][i];

                if (!layerHF.valid())
                {
                    // We couldn't get the heightfield from the cache, so try to create it.
                    // We also fallback on parent layers to make sure that we have data at the location even if it's fallback.
                    while (!layerHF.valid() && actualKey.valid() && layer->isKeyInLegalRange(actualKey))
                    {
                        layerHF = layer->createHeightField(actualKey, progress);
                        if (!layerHF.valid())
                        {
                            actualKey = actualKey.createParentKey();
                        }
                    }

                    // Mark this layer as fallback if necessary.
                    if (layerHF.valid())
                    {
                        heightFallback[n][i] = (actualKey != contenderKey); // actualKey != contenders[i].second;
                        numHeightFieldsInCache++;
                    }
                    else
                    {
                        heightFailed[n][i] = true;
                        continue;
                    }
                }

                if (layerHF.valid())
                {
                    bool isFallback = heightFallback[n][i];

                    // We only have real data if this is not a fallback heightfield.
                    if (!isFallback)
                    {
                        realData = true;
                    }
                    
                    float elevation;
                    if (layerHF.getElevation(keySRS, x, y, interpolation, keySRS, elevation))
                    {
                        if ( elevation != NO_DATA_VALUE )
                        {
                            // remember the index so we can only apply offset layers that
                            // sit on TOP of this layer.
                            resolvedIndex = index;

                            hf->setHeight(c, r, elevation);

                            if (deltaLOD)
                            {
                                (*deltaLOD)[r*numColumns + c] = key.getLOD() - actualKey.getLOD();
                            }
                        }
                        else
                        {
                            ++nodataCount;
                        }
                    }                    
                }


                // Clear the heightfield cache if we have too many heightfields in the cache.
                if (numHeightFieldsInCache >= maxHeightFields)
                {
                    //OE_NOTICE << "Clearing cache" << std::endl;
                    for (unsigned int j = 0; j < 9; ++j)
                    {
                        for (unsigned int k = 0; k < heightFields[j].size(); k++)
                        {
                            heightFields[j][k] = GeoHeightField::INVALID;
                            heightFallback[j][k] = false;
                        }
                    }
                    numHeightFieldsInCache = 0;
                }
            }

            for(int i=offsets.size()-1; i>=0; --i)
            {
                // Only apply an offset layer if it sits on top of the resolved layer
                // (or if there was no resolved layer).
                if (resolvedIndex >= 0 && offsets[i].index < resolvedIndex)
                    continue;

                TileKey &contenderKey = offsets[i].key;

                // If there is a border, the edge points may not fall within the key extents 
                // and we may need to fetch a neighboring key.

                int n = 4; // index 4 is the center/default tile

#if 0
                if (border > 0u && !contenderKey.getExtent().contains(x, y))
                {
                    int dTx = x < contenderKey.getExtent().xMin() ? -1 : x > contenderKey.getExtent().xMax() ? +1 : 0;
                    int dTy = y < contenderKey.getExtent().yMin() ? +1 : x > contenderKey.getExtent().yMax() ? -1 : 0;
                    contenderKey = contenderKey.createNeighborKey(dTx, dTy);
                    n = (dTy+1)*3 + (dTx+1);
                }
#endif
                
                if ( offsetFailed[n][i] == true )
                    continue;

                GeoHeightField& layerHF = offsetFields[n][i];
                if ( !layerHF.valid() )
                {
                    ElevationLayer* offset = offsets[i].layer.get();

                    layerHF = offset->createHeightField(contenderKey, progress);
                    if ( !layerHF.valid() )
                    {
                        offsetFailed[n][i] = true;
                        continue;
                    }
                }

                // If we actually got a layer then we have real data
                realData = true;

                float elevation = 0.0f;
                if (layerHF.getElevation(keySRS, x, y, interpolation, keySRS, elevation) &&
                    elevation != NO_DATA_VALUE)
                {                    
                    hf->getHeight(c, r) += elevation;

                    // Update the resolution tracker to account for the offset. Sadly this
                    // will wipe out the resolution of the actual data, and might result in 
                    // normal faceting. See the comments on "createNormalMap" for more info
                    if (deltaLOD)
                    {
                        (*deltaLOD)[r*numColumns + c] = key.getLOD() - contenderKey.getLOD();
                    }
                }
            }
        }
    }

    if (normalMap)
    {
        createNormalMap(key.getExtent(), hf, deltaLOD.get(), normalMap);
    }

    // Return whether or not we actually read any real data
    return realData;
}
