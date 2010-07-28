
/* Copyright (c) 2009-2010, Cedric Stalder <cedric.stalder@gmail.com> 
 *               2009-2010, Stefan Eilemann <eile@equalizergraphics.com>
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

#ifndef EQBASE_COMPRESSOR_H
#define EQBASE_COMPRESSOR_H

#include <eq/plugins/compressor.h> // member
#include <eq/base/base.h>
#include <eq/base/dso.h>           // member

/**
 * @file base/compressor.h
 * 
 * Helper class for storing the function pointers of one compressor plugin DSO.
 * @internal
 */
namespace eq
{
namespace base
{
    /**
     * A class holding all functions and information for one compressor DSO.
     * @internal
     */
    class Compressor
    {
    public:
        typedef size_t ( *GetNumCompressors_t ) ();
        typedef void   ( *GetInfo_t ) ( const size_t, EqCompressorInfo* const );
        typedef void*  ( *NewCompressor_t ) ( const unsigned );
        typedef void   ( *DeleteCompressor_t ) ( void* const );
        typedef void*  ( *NewDecompressor_t ) ( const unsigned );
        typedef void   ( *DeleteDecompressor_t ) ( void* const );
        typedef void   ( *Compress_t ) ( void* const, const unsigned, 
                                         void* const, const uint64_t*,
                                         const uint64_t );
        typedef size_t ( *GetNumResults_t ) ( void* const, const unsigned );
        typedef void   ( *GetResult_t ) ( void* const, const unsigned, 
                                          const unsigned, void** const, 
                                          uint64_t* const );
        typedef void   ( *Decompress_t ) ( void* const, const unsigned,
                                           const void* const*,
                                           const uint64_t* const,
                                           const unsigned, void* const, 
                                           uint64_t* const, 
                                           const uint64_t );
        typedef bool ( *IsCompatible_t ) ( const unsigned, const GLEWContext* );
        typedef void ( *Download_t )( void* const, const unsigned, 
                                      const GLEWContext*, const uint64_t*,
                                      const unsigned, const uint64_t,
                                      uint64_t*, void** );
        typedef void ( *Upload_t )( void* const, const unsigned, 
                                    const GLEWContext*, const void*,
                                    const uint64_t*,
                                    const uint64_t, const uint64_t*,
                                    const unsigned  );

        Compressor(){}

        /** Init and link a compressor plugin. */
        bool init( const std::string& libraryName );
      
        /** Unload a compressor plugin. */
        void exit();

        /** Get the number of compressor found in the plugin. */
        GetNumCompressors_t  getNumCompressors;

        /** Get a new compressor instance.  */
        NewCompressor_t newCompressor;
        
        /** Get a new decompressor instance.  */
        NewDecompressor_t    newDecompressor;
       
        /** Delete the compressor instance.  */     
        DeleteCompressor_t   deleteCompressor;
        
        /** Delete the decompressor instance.  */ 
        DeleteDecompressor_t deleteDecompressor;
      
        /** Compress data. */
        Compress_t       compress;

        /** Decompress data. */
        Decompress_t     decompress;
      
        /** Get the number of results from the last compression.  */
        GetNumResults_t  getNumResults;

        /** Get the nth result from the last compression.  */
        GetResult_t   getResult;

        /** Check if the transfer plugin can be used. */
        IsCompatible_t isCompatible;

        /** Download pixel data. */
        Download_t  download;

        /** Upload pixel data. */
        Upload_t  upload;

        /** @return true if name is found in the DSO compressor */
        bool implementsType( const uint32_t name ) const;

        /** @return the information for all compressors contained in the DSO. */
        const CompressorInfos& getInfos() const { return _infos; }

        /** @return the information for the given compressor, or 0. */
        const EqCompressorInfo* findInfo( const uint32_t name ) const;

    private:
        CompressorInfos _infos;
        base::DSO _dso;   
    };

    EQ_EXPORT std::ostream& operator << ( std::ostream&, 
                                          const EqCompressorInfo& );
}
}

#endif //EQ_COMPRESSOR_H
