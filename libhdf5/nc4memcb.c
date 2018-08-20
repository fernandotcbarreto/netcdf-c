/*********************************************************************
*    Copyright 2018, UCAR/Unidata
*    See netcdf/COPYRIGHT file for copying and redistribution conditions.
* ********************************************************************/

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
* Copyright by The HDF Group.                                               *
* Copyright by the Board of Trustees of the University of Illinois.         *
* All rights reserved.                                                      *
*                                                                           *
* This file is part of HDF5.  The full HDF5 copyright notice, including     *
* terms governing use, modification, and redistribution, is contained in    *
 * the COPYING file, which can be found at the root of the source code       *
 * distribution tree, or in https://support.hdfgroup.org/ftp/HDF5/releases.  *
 * If you do not have access to either file, you may request a copy from     *
 * help@hdfgroup.org.                                                        *
* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * @internal Inmemory support
 *
 * This code is derived from H5LT.c#H5LTopen_file_image.
 * In order to make the netcdf inmemory code work, it is necessary
 * to modify some of the callback functions; specifically
 * image_malloc, image_realloc, and image_memcpy.
 * <p>
 * The changes are directed at allowing the caller to
 * specify two things.
 * <ol>
 * <li> To specify (indirectly) the H5LT_FILE_IMAGE_DONT_COPY flag.
 * This means that no attempt to realloc the caller provided memory
 * should be made. This also means that the memory block pointer
 * provided by the caller will be thesame returned by <em>nc_close_memio()</em>.
 * <li> The caller overallocates the memory so that there is space
 * to allow the file to be modified in place.
 * </ol>
 * <p>
 * The existing implementation of H5LTopen_file_image has two flaws
 * with respect to these properties.
 * <ol>
 * <li> The image_realloc callback fails if
 * H5LT_FILE_IMAGE_DONT_COPY flag is set even if there is room
 * to allow the memory block to pretend to expand (because
 * of overallocation).
 * <li> When the caller attempts to get the final memory block,
 * the HDF5 library makes a copy, unless theH5LT_FILE_IMAGE_DONT_COPY
 * flag is set. This is unnecessary. Note that in this situation,
 * the HDF5 library will use
 * <em>image_malloc()</em>
 * followed by
 * <em>image_memcpy()</em>
 * </ol>
 * <p>
 * So, the callback changes to support this properly are as follows.
 * <dl>
 * <dt>image_realloc</dt>
 * <dd>If there is sufficient space (because of overallocation),
 * the pretend to realloc and return the incoming memory block
 * instead of taking the chance of doing a real realloc.</dd>
 * <dt>image_malloc</dt>
 * <dd>If the operation being performed is to obtain
 * the space to copy the final memory, then just return
 * the original memory block. Note that this case is detectable
 * because the callback is given the value
 * H5FD_FILE_IMAGE_OP_PROPERTY_LIST_GET.</dd>
 * <dt>image_memcpy</dt>
 * <dd>Similar to the image_malloc change.
 * Namely, if the operation being performed is to copy
 * out the final memory contents, and the final memory block
 * is the same as that provided by the caller originally, then
 * just do nothing. Again, this case can be detected
 * by the occurrence of H5FD_FILE_IMAGE_OP_PROPERTY_LIST_GET.</dd>
 * </dl>
 * @author Dennis Heimbigner
*/

#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>

#include <hdf5.h>
#include <hdf5_hl.h>

#include "nc4internal.h"

#ifndef HDrealloc
#define HDrealloc(x,y) realloc(x,y)
#endif
#ifndef SUCCEED
#define SUCCEED 0
#define FAIL -1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#define TRACE
#define CATCH
#define TRACE_UDATA

#ifdef TRACE
#define CATCH
#endif

#ifdef TRACE
#include <stdarg.h>
static void trace(const char* fcn, H5FD_file_image_op_t op, void* _udata, ...);
static void traceend(const char* fcn, void* _udata, uintptr_t retval);
static const char* traceop(H5FD_file_image_op_t op);
static void traceflags(int flags);

/* In case we do not have variadic macros */
#define TRACE0(fcn,op,udata)  trace(fcn,op,udata)
#define TRACE1(fcn,op,udata,x1)  trace(fcn,op,udata,x1)
#define TRACE2(fcn,op,udata,x1,x2)  trace(fcn,op,udata,x1,x2)
#define TRACE3(fcn,op,udata,x1,x2,x3)  trace(fcn,op,udata,x1,x2,x3)
#define TRACEEND(fcn,udata,retval) traceend(fcn,udata,(uintptr_t)retval);
#else /*!TRACE*/
#define TRACE0(fcn,op,udata)
#define TRACE1(fcn,op,udata,x1)
#define TRACE2(fcn,op,udata,x1,x2)
#define TRACE3(fcn,op,udata,x1,x2,x3)
#define TRACEEND(fcn,udata,retval)
#endif

#ifdef CATCH
static void tracefail(const char* fcn);
#define TRACEFAIL(fcn) tracefail(fcn)
#else
#define TRACEFAIL(fcn)
#endif

#define DEFAULT_CREATE_MEMSIZE ((size_t)1<<16)

#ifndef H5LT_FILE_IMAGE_DONT_COPY

/* Flag definitions for H5LTopen_file_image() */
#define H5LT_FILE_IMAGE_OPEN_RW      0x0001 /* Open image for read-write */
#define H5LT_FILE_IMAGE_DONT_COPY    0x0002 /* The HDF5 lib won't copy   */
/* user supplied image buffer. The same image is open with the core driver.  */
#define H5LT_FILE_IMAGE_DONT_RELEASE 0x0004 /* The HDF5 lib won't        */
/* deallocate user supplied image buffer. The user application is reponsible */
/* for doing so.                                                             */
#define H5LT_FILE_IMAGE_ALL          0x0007

#endif /*H5LT_FILE_IMAGE_DONT_COPY*/


#if 0
/* For Lex and Yacc */
#define         COL             3
#define         LIMIT           512
#define         INCREMENT       1024
#define         TMP_LEN         256
#define         MAX(a,b)        (((a)>(b)) ? (a) : (b))
size_t  input_len;
char *myinput;
size_t  indent = 0;
#endif /*0*/

/* File Image operations

   A file image is a representation of an HDF5 file in a memory
   buffer. In order to perform operations on an image in a similar way
   to a  file, the application buffer is copied to a FAPL buffer, which
   in turn is copied to a VFD buffer. Buffer copying can decrease
   performance, especially when using large file images. A solution to
   this issue is to simulate the copying of the application buffer,
   when actually the same buffer is used for the FAPL and the VFD.
   This is implemented by using callbacks that simulate the standard
   functions for memory management (additional callbacks are used for
   the management of associated data structures). From the application
   standpoint, a file handle can be obtained from a file image by using
   the API routine H5LTopen_file_image(). This function takes a flag
   argument that indicates the HDF5 library how to handle the given image;
   several flag values can be combined by using the bitwise OR operator.
   Valid flag values include:

   H5LT_FILE_IMAGE_OPEN_RW indicates the HDF5 library to open the file
   image in read/write mode. Default is read-only mode.

   H5LT_FILE_IMAGE_DONT_COPY indicates the HDF5 library to not copy the
   supplied user buffer; the same buffer will be handled by the FAPL and
   the VFD driver. Default operation copies the user buffer to the FAPL and
   VFD driver.

   H5LT_FILE_IMAGE_DONT_RELEASE indicates the HDF5 library to not release
   the buffer handled by the FAPL and the VFD upon closing. This flag value
   is only applicable when the flag value H5LT_FILE_IMAGE_DONT_COPY is set as
   well. The application is responsible to release the image buffer.
*/

/* Data structure to pass application data to callbacks. */
/* Modified to add NC_FILE_INFO_T ptr */
typedef struct {
    void *app_image_ptr;	/* Pointer to application buffer */
    size_t app_image_size;	/* Size of application buffer */
    void *fapl_image_ptr;	/* Pointer to FAPL buffer */
    size_t fapl_image_size;	/* Size of FAPL buffer */
    int fapl_ref_count;		/* Reference counter for FAPL buffer */
    void *vfd_image_ptr;	/* Pointer to VFD buffer */
    size_t vfd_image_size;	/* Size of VFD buffer */
    int vfd_ref_count;		/* Reference counter for VFD buffer */
    unsigned flags;		/* Flags indicate how the file image will */
                                /* be open */
    int ref_count;		/* Reference counter on udata struct */
    NC_FILE_INFO_T* h5;
} H5LT_file_image_ud_t;

/* callbacks prototypes for file image ops */
static void *local_image_malloc(size_t size, H5FD_file_image_op_t file_image_op, void *udata);
static void *local_image_memcpy(void *dest, const void *src, size_t size, H5FD_file_image_op_t file_image_op, void *udata);
static herr_t local_image_free(void *ptr, H5FD_file_image_op_t file_image_op, void *udata);
static void *local_udata_copy(void *udata);
static herr_t local_udata_free(void *udata);

/* Definition of callbacks for file image operations. */


/*-------------------------------------------------------------------------
* Function: image_malloc
*
* Purpose: Simulates malloc() function to avoid copying file images.
*          The application buffer is set to the buffer on only one FAPL.
*          Then the FAPL buffer can be copied to other FAPL buffers or
*          to only one VFD buffer.
*
* Return: Address of "allocated" buffer, if successful. Otherwise, it returns
*         NULL.
*
* Programmer: Christian Chilan
*
* Date: October 3, 2011
*
*-------------------------------------------------------------------------
*/
static void *
local_image_malloc(size_t size, H5FD_file_image_op_t file_image_op, void *_udata)
{
    H5LT_file_image_ud_t *udata = (H5LT_file_image_ud_t *)_udata;
    void * return_value = NULL;

    TRACE1("malloc", file_image_op, _udata, size);

#if 0
    /* callback is only used if the application buffer is not actually copied */
    if (!(udata->flags & H5LT_FILE_IMAGE_DONT_COPY))
        goto out;
#endif

    switch ( file_image_op ) {
        /* the app buffer is "copied" to only one FAPL. Afterwards, FAPLs can be "copied" */
        case H5FD_FILE_IMAGE_OP_PROPERTY_LIST_SET:
	    if (!(udata->flags & H5LT_FILE_IMAGE_DONT_COPY)) { /* Do a malloc */
                if (udata->app_image_ptr == NULL) {
	            if(NULL == (udata->app_image_ptr = malloc(size)))
		        LOG((0,"image_malloc: unable to allocate memory block"));
#ifdef TRACE
		    fprintf(stderr,"\t>>>> malloc(%ld)=>%p\n",
				(unsigned long)size,udata->app_image_ptr);
#endif
		    udata->app_image_size = size;
		}
	    }
            if (udata->app_image_ptr == NULL)
                goto out;
            if (udata->app_image_size != size)
                goto out;
	    if (udata->fapl_image_ptr != NULL)
                goto out;
            if (udata->fapl_image_size != 0)
                goto out;
            if (udata->fapl_ref_count != 0)
                goto out;

            udata->fapl_image_ptr = udata->app_image_ptr;
            udata->fapl_image_size = udata->app_image_size;
            return_value = udata->fapl_image_ptr;
            udata->fapl_ref_count++;
	    break;

	case H5FD_FILE_IMAGE_OP_PROPERTY_LIST_COPY:
            if (udata->fapl_image_ptr == NULL)
                goto out;
            if (udata->fapl_image_size != size)
                goto out;
            if (udata->fapl_ref_count == 0)
                goto out;

            return_value = udata->fapl_image_ptr;
            udata->fapl_ref_count++;
	    break;

        case H5FD_FILE_IMAGE_OP_PROPERTY_LIST_GET:
            if (udata->fapl_image_ptr == NULL)
                goto out;
	    /* fake the malloc by returning the current memory */
            return_value = udata->fapl_image_ptr;
	    break;

        case H5FD_FILE_IMAGE_OP_FILE_OPEN:
            /* FAPL buffer is "copied" to only one VFD buffer */
            if (udata->vfd_image_ptr != NULL)
                goto out;
            if (udata->vfd_image_size != 0)
                goto out;
            if (udata->vfd_ref_count != 0)
                goto out;
            if (udata->fapl_image_ptr == NULL)
                goto out;
            if (udata->fapl_image_size != size)
                goto out;
            if (udata->fapl_ref_count == 0)
                goto out;

            udata->vfd_image_ptr = udata->fapl_image_ptr;
 	    udata->vfd_image_size = size;
            udata->vfd_ref_count++;
            return_value = udata->vfd_image_ptr;
            break;

	/* added unused labels to shut the compiler up */
	case H5FD_FILE_IMAGE_OP_NO_OP:
	case H5FD_FILE_IMAGE_OP_PROPERTY_LIST_CLOSE:
	case H5FD_FILE_IMAGE_OP_FILE_RESIZE:
	case H5FD_FILE_IMAGE_OP_FILE_CLOSE:
        default:
            goto out;
    } /* end switch */

    TRACEEND("malloc",_udata,return_value);

    return(return_value);

out:
    TRACEFAIL("malloc");
    return NULL;
} /* end image_malloc() */


/*-------------------------------------------------------------------------
* Function: image_memcpy
*
* Purpose:  Simulates memcpy() function to avoid copying file images.
*           The image buffer can be set to only one FAPL buffer, and
*           "copied" to only one VFD buffer. The FAPL buffer can be
*           "copied" to other FAPLs buffers.
*
* Return: The address of the destination buffer, if successful. Otherwise, it
*         returns NULL.
*
* Programmer: Christian Chilan
*
* Date: October 3, 2011
*
*-------------------------------------------------------------------------
*/
static void *
local_image_memcpy(void *dest, const void *src, size_t size, H5FD_file_image_op_t file_image_op,
    void *_udata)
{
    H5LT_file_image_ud_t *udata = (H5LT_file_image_ud_t *)_udata;

    TRACE3("memcpy", file_image_op, _udata,dest,src,size);

#if 0
    /* callback is only used if the application buffer is not actually copied */
    if (!(udata->flags & H5LT_FILE_IMAGE_DONT_COPY))
        goto out;
#endif

    switch(file_image_op) {
        case H5FD_FILE_IMAGE_OP_PROPERTY_LIST_SET:
            if (dest != udata->fapl_image_ptr)
                goto out;
            if (src != udata->app_image_ptr)
                goto out;
            if (size != udata->fapl_image_size)
                goto out;
            if (size != udata->app_image_size)
                goto out;
            if (udata->fapl_ref_count == 0)
                goto out;
            if (!(udata->flags & H5LT_FILE_IMAGE_DONT_COPY)) {
		if(src != dest) {
		    memcpy(dest,src,size);
#ifdef TRACE
		    fprintf(stderr,"\t>>>> memcpy(%p,%p,%ld)\n",
				dest,src,(unsigned long)size);
#endif
		}
	    }
            break;

        case H5FD_FILE_IMAGE_OP_PROPERTY_LIST_COPY:
            if (dest != udata->fapl_image_ptr)
                goto out;
            if (src != udata->fapl_image_ptr)
                goto out;
            if (size != udata->fapl_image_size)
                goto out;
            if (udata->fapl_ref_count < 2)
                goto out;
            break;

        case H5FD_FILE_IMAGE_OP_PROPERTY_LIST_GET:
	    if (!(udata->flags & H5LT_FILE_IMAGE_DONT_COPY))
                goto out;
	    /* test: src == dest == original */
	    if(src != dest || src != udata->fapl_image_ptr)
		goto out;
	    break;

        case H5FD_FILE_IMAGE_OP_FILE_OPEN:
            if (dest != udata->vfd_image_ptr)
                goto out;
            if (src != udata->fapl_image_ptr)
                goto out;
            if (size != udata->vfd_image_size)
                goto out;
            if (size != udata->fapl_image_size)
                goto out;
            if (udata->fapl_ref_count == 0)
                goto out;
            if (udata->vfd_ref_count != 1)
                goto out;
            break;

	/* added unused labels to shut the compiler up */
	case H5FD_FILE_IMAGE_OP_NO_OP:
	case H5FD_FILE_IMAGE_OP_PROPERTY_LIST_CLOSE:
	case H5FD_FILE_IMAGE_OP_FILE_RESIZE:
	case H5FD_FILE_IMAGE_OP_FILE_CLOSE:
        default:
            goto out;
    } /* end switch */

    TRACEEND("memcpy",_udata,dest);
    return(dest);

out:
    TRACEFAIL("memcpy");
    return NULL;
} /* end image_memcpy() */


/*-------------------------------------------------------------------------
* Function: image_realloc
*
* Purpose: Reallocates the shared application image buffer and updates data
*          structures that manage buffer "copying".
*
* Return: Address of reallocated buffer, if successful. Otherwise, it returns
*         NULL.
*
* Programmer: Christian Chilan
*
* Date: October 3, 2011
*
*-------------------------------------------------------------------------
*/
static void *
local_image_realloc(void *ptr, size_t size, H5FD_file_image_op_t file_image_op, void *_udata)
{
    H5LT_file_image_ud_t *udata = (H5LT_file_image_ud_t *)_udata;
    void * return_value = NULL;

    TRACE2("realloc", file_image_op, _udata, ptr, size);

#if 0
    /* callback is only used if the application buffer is not actually copied */
    if (!(udata->flags & H5LT_FILE_IMAGE_DONT_COPY))
        goto out;
#endif

    /* realloc() is not allowed if the image is open in read-only mode */
    if (!(udata->flags & H5LT_FILE_IMAGE_OPEN_RW))
        goto out;

#if 0
    /* realloc() is not allowed when the HDF5 library won't release the image 
       buffer because reallocation may change the address of the buffer. The
       new address cannot be communicated to the application to release it. */
    if (udata->flags & H5LT_FILE_IMAGE_DONT_RELEASE) 
        goto out; 
#endif

    if (file_image_op == H5FD_FILE_IMAGE_OP_FILE_RESIZE) {
        if (udata->vfd_image_ptr != ptr)
            goto out;

        if (udata->vfd_ref_count != 1)
            goto out;

        if (!(udata->flags & H5LT_FILE_IMAGE_DONT_COPY)
	    && !(udata->flags & H5LT_FILE_IMAGE_DONT_RELEASE)) {
	    void* oldvfd = udata->vfd_image_ptr;
	    assert(ptr == oldvfd); /* validate */
	    if(NULL == (udata->vfd_image_ptr = realloc(ptr, size))) {
		LOG((0,"image_realloc: unable to allocate memory block of size: %lu bytes",(unsigned long)size));
		goto out;
	    }
#ifdef TRACE
	    fprintf(stderr,"\t>>>> realloc(%p,%ld)=>%p\n",ptr,(unsigned long)size,udata->vfd_image_ptr);
#endif
           udata->vfd_image_size = size;
	   /* Record that we have new memory block */
	   assert(oldvfd == udata->h5->mem.memio.memory);
	   udata->h5->mem.memio.memory = udata->vfd_image_ptr;
           return_value = udata->vfd_image_ptr;
	} else {
	   /* Modified:
           1. If the realloc new size is <= existing size,
	      then pretend we did a realloc and return success.
              This avoids unneccessary heap operations.
           2. If the H5LT_FILE_IMAGE_DONT_COPY or
	      H5LT_FILE_IMAGE_DONT_RELEASE flag is set and the
	      realloc new size is > existing size, then fail
	      because the realloc() call may change the address
	      of the buffer. The new address cannot be
	      communicated to the application to release it.
	   3. Otherwise, use realloc(). Note that this may have the
              side effect of freeing the previous memory chunk.
           */
	   if(size <= udata->vfd_image_size) {
	       /* Ok, pretend we did a realloc but just change size*/
               udata->vfd_image_size = size;
           } else
		goto out;
        }
        return_value = udata->vfd_image_ptr;
    } /* end if */
    else
        goto out;

    TRACEEND("realloc",_udata,return_value);
    return(return_value);

out:
    TRACEFAIL("realloc");
    return NULL;
} /* end local_image_realloc() */

/*-------------------------------------------------------------------------
* Function: image_free
*
* Purpose: Simulates deallocation of FAPL and VFD buffers by decreasing
*          reference counters. Shared application buffer is actually
*          deallocated if there are no outstanding references.
*
* Return: SUCCEED or FAIL
*
* Programmer: Christian Chilan
*
* Date: October 3, 2011
*
*-------------------------------------------------------------------------
*/
static herr_t
local_image_free(void *ptr, H5FD_file_image_op_t file_image_op, void *_udata)
{
    H5LT_file_image_ud_t *udata = (H5LT_file_image_ud_t *)_udata;

    TRACE1("free", file_image_op, _udata, ptr);

#if 0
    /* callback is only used if the application buffer is not actually copied */
    if (!(udata->flags & H5LT_FILE_IMAGE_DONT_COPY))
        goto out;
#endif

    /* As a rule, we never free a buffer, but instead
	either do nothing or save it in h5->mem.memio
    */
    switch(file_image_op) {
        case H5FD_FILE_IMAGE_OP_PROPERTY_LIST_CLOSE:
	    if (udata->fapl_image_ptr != ptr)
                goto out;
            if (udata->fapl_ref_count == 0)
                goto out;

            udata->fapl_ref_count--;

            /* free the shared buffer only if indicated by the respective flag
	       and there are no outstanding references */
            if (udata->fapl_ref_count == 0 && udata->vfd_ref_count == 0) {
                udata->app_image_ptr = NULL;
                udata->fapl_image_ptr = NULL;
                udata->vfd_image_ptr = NULL;
            } /* end if */
            break;

        case H5FD_FILE_IMAGE_OP_FILE_CLOSE:
            if (udata->vfd_image_ptr != ptr)
                goto out;
            if (udata->vfd_ref_count != 1)
                goto out;

            udata->vfd_ref_count--;

            if (udata->fapl_ref_count == 0 && udata->vfd_ref_count == 0) {
	        /* It should be the case that udata->h5->mem.memio.memory === udata->vfd_image_ptr */
		assert(udata->h5->mem.memio.memory == udata->vfd_image_ptr);
		/* cleanup */
                udata->app_image_ptr = NULL;
                udata->fapl_image_ptr = NULL;
                udata->vfd_image_ptr = NULL;
            } /* end if */
            break;

	/* added unused labels to keep the compiler quite */
	case H5FD_FILE_IMAGE_OP_NO_OP:
	case H5FD_FILE_IMAGE_OP_PROPERTY_LIST_SET:
	case H5FD_FILE_IMAGE_OP_PROPERTY_LIST_COPY:
	case H5FD_FILE_IMAGE_OP_PROPERTY_LIST_GET:
	case H5FD_FILE_IMAGE_OP_FILE_OPEN:
	case H5FD_FILE_IMAGE_OP_FILE_RESIZE:
	default:
            goto out;
    } /* end switch */

    TRACEEND("free",_udata,1);
    return(SUCCEED);

out:
    TRACEFAIL("free");
    return(FAIL);
} /* end image_free() */


/*-------------------------------------------------------------------------
* Function: udata_copy
*
* Purpose: Simulates the copying of the user data structure utilized in the
*          management of the "copying" of file images.
*
* Return: Address of "newly allocated" structure, if successful. Otherwise, it
*         returns NULL.
*
* Programmer: Christian Chilan
*
* Date: October 3, 2011
*
*-------------------------------------------------------------------------
*/
static void *
local_udata_copy(void *_udata)
{
    H5LT_file_image_ud_t *udata = (H5LT_file_image_ud_t *)_udata;

    TRACE0("udata_copy", 0,  _udata);

#if 0
    /* callback is only used if the application buffer is not actually copied */
    if (!(udata->flags & H5LT_FILE_IMAGE_DONT_COPY))
        goto out;
#endif

    /* never copy */
   if (udata->ref_count == 0)
	goto out;
    udata->ref_count++;

    TRACEEND("udata_copy",udata,1);
    return(udata);

out:
    TRACEFAIL("udata_copy");
    return NULL;
} /* end udata_copy */


/*-------------------------------------------------------------------------
* Function: udata_free
*
* Purpose: Simulates deallocation of the user data structure utilized in the
*          management of the "copying" of file images. The data structure is
*          actually deallocated when there are no outstanding references.
*
* Return: SUCCEED or FAIL
*
* Programmer: Christian Chilan
*
* Date: October 3, 2011
*
*-------------------------------------------------------------------------
*/
static herr_t
local_udata_free(void *_udata)
{
    H5LT_file_image_ud_t *udata = (H5LT_file_image_ud_t *)_udata;

    TRACE0("udata_free", 0, _udata);

#if 0
    /* callback is only used if the application buffer is not actually copied */
    if (!(udata->flags & H5LT_FILE_IMAGE_DONT_COPY))
        goto out;
#endif

    if (udata->ref_count == 0)
        goto out;
    udata->ref_count--;

    /* checks that there are no references outstanding before deallocating udata */
    if (udata->ref_count == 0 && udata->fapl_ref_count == 0 && udata->vfd_ref_count == 0) {
        free(udata);
#ifdef TRACE_UDATA
	fprintf(stderr,"\t>>>> freed: udata=%p\n",udata);
#endif
	udata = NULL;	
    }
    TRACEEND("udata_free",udata,1);
    return(SUCCEED);

out:
    TRACEFAIL("udata_free");
    return(FAIL);
} /* end udata_free */


/* End of callbacks definitions for file image operations */

hid_t
NC4_image_init(NC_FILE_INFO_T* h5)
{
    hid_t		fapl = -1, file_id = -1; /* HDF5 identifiers */
    unsigned            file_open_flags = 0;/* Flags for hdf5 open */
    char                file_name[64];	/* Filename buffer */
    size_t              alloc_incr;     /* Buffer allocation increment */
    size_t              min_incr = 65536; /* Minimum buffer increment */
    double              buf_prcnt = 0.1f;  /* Percentage of buffer size to set
                                             as increment */
    size_t buf_size = 0;
    void* buf_ptr = NULL;
    unsigned imageflags;
    int create = 0;
    H5LT_file_image_ud_t *udata = NULL;	/* Pointer to udata structure */

    H5FD_file_image_callbacks_t callbacks = {&local_image_malloc, &local_image_memcpy,
                                           &local_image_realloc, &local_image_free,
                                           &local_udata_copy, &local_udata_free,
                                           (void *)NULL};
    static long         file_name_counter;

    imageflags = h5->mem.imageflags;
    create = h5->mem.created;

    /* check arguments */
    if (h5->mem.memio.memory == NULL) {
	if(create) {
	    if(h5->mem.memio.size == 0) h5->mem.memio.size = DEFAULT_CREATE_MEMSIZE;
	    h5->mem.memio.memory = malloc(h5->mem.memio.size);
	} else
	    goto out; /* open requires an input buffer */
    } else if(h5->mem.memio.size == 0)
	goto out;

    /* alias for convenience */
    buf_size = h5->mem.memio.size;
    buf_ptr = h5->mem.memio.memory;

    /* Create FAPL to transmit file image */
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0)
        goto out;

    /* set allocation increment to a percentage of the supplied buffer size, or
     * a pre-defined minimum increment value, whichever is larger
     */
    if ((buf_prcnt * buf_size) > min_incr)
        alloc_incr = (size_t)(buf_prcnt * buf_size);
    else
        alloc_incr = min_incr;

    /* Configure FAPL to use the core file driver */
    if (H5Pset_fapl_core(fapl, alloc_incr, FALSE) < 0)
        goto out;

    /* Set callbacks for file image ops always */
    {
        /* Allocate buffer to communicate user data to callbacks */
        if (NULL == (udata = (H5LT_file_image_ud_t *)calloc(1,sizeof(H5LT_file_image_ud_t))))
            goto out;

        /* Initialize udata with info about app buffer containing file image  and flags */
        udata->app_image_ptr = buf_ptr;
        udata->app_image_size = buf_size;
        udata->fapl_image_ptr = NULL;
        udata->fapl_image_size = 0;
        udata->fapl_ref_count = 0;
        udata->vfd_image_ptr = NULL;
        udata->vfd_image_size = 0;
        udata->vfd_ref_count = 0;
        udata->flags = imageflags;
        udata->ref_count = 1; /* corresponding to the first FAPL */
	udata->h5 = h5;

        /* copy address of udata into callbacks */
        callbacks.udata = (void *)udata;

        /* Set file image callbacks */
        if (H5Pset_file_image_callbacks(fapl, &callbacks) < 0)
            goto out;
    }

    /* Assign file image in user buffer to FAPL */
    if (H5Pset_file_image(fapl, buf_ptr, buf_size) < 0)
        goto out;

    /* define a unique file name */
    snprintf(file_name, (sizeof(file_name) - 1), "file_image_%ld", file_name_counter++);

    /* set file open/create flags */
    if(create)
        file_open_flags = H5F_ACC_TRUNC; /* H5Fcreate does not like H5F_ACC_RDWR */
    else if (imageflags & H5LT_FILE_IMAGE_OPEN_RW)
        file_open_flags = H5F_ACC_RDWR;
    else
        file_open_flags = H5F_ACC_RDONLY;

    /* Assign file image in FAPL to the core file driver */
    if(create) {
        if ((file_id = H5Fcreate(file_name, file_open_flags, H5P_DEFAULT, fapl)) < 0)
            goto out;
    } else {
        if ((file_id = H5Fopen(file_name, file_open_flags, fapl)) < 0)
            goto out;
    }

    h5->mem.udata = udata;

done:
    /* Reclaim the fapl object */
    H5E_BEGIN_TRY {
	if(fapl >= 0)
            H5Pclose(fapl);
    } H5E_END_TRY;
    /* Reclaim udata */
    /* Return file identifier */
    return file_id;

out:
    if(udata != NULL) free(udata);
    file_id = -1;
    goto done;
} /* end H5LTopen_file_image() */

void
NC4_image_finalize(void* _udata)
{
    if(_udata != NULL) {
	H5LT_file_image_ud_t *udata = (H5LT_file_image_ud_t*)_udata;
#if 0
	if(udata->app_image_ptr != NULL) free(udata->app_image_ptr);
	if(udata->fapl_image_ptr != NULL) free(udata->fapl_image_ptr);
	if(udata->vfd_image_ptr != NULL) free(udata->vfd_image_ptr);
#endif
	free(udata);
    }    
}

#ifdef TRACE

static void
printudata(H5LT_file_image_ud_t* udata)
{
    if(udata == NULL) return;
    fprintf(stderr," flags=");
    traceflags(udata->flags);
    fprintf(stderr,"\n\t    ref_count=%d",udata->ref_count);
    fprintf(stderr,"\n\t    app=(%p,%lld)",udata->app_image_ptr,(long long)udata->app_image_size);
    fprintf(stderr,"\n\t    fapl=(%p,%lld)[%d]",udata->fapl_image_ptr,(long long)udata->fapl_image_size,udata->fapl_ref_count);
    fprintf(stderr,"\n\t    vfd=(%p,%lld)[%d]",udata->vfd_image_ptr,(long long)udata->vfd_image_size,udata->vfd_ref_count);
}

static void
trace(const char* fcn, H5FD_file_image_op_t op, void* _udata, ...)
{
    H5LT_file_image_ud_t *udata = NULL;
    va_list ap;

    va_start(ap, _udata); /* Requires the last fixed parameter (to get the address) */
    udata = (H5LT_file_image_ud_t *)_udata;
    if(strcmp("malloc",fcn)==0) {
	size_t size = va_arg(ap,size_t);
	fprintf(stderr,"trace: %s: op=%s: size=%lld\n\tudata=",
		fcn,traceop(op),(long long)size);
	printudata(udata);
        fprintf(stderr,"\n");
    } else if(strcmp("realloc",fcn)==0) {
	void* ptr = va_arg(ap,void*);
	size_t size = va_arg(ap,size_t);
	fprintf(stderr,"trace: %s: op=%s: ptr=%p, size=%lld\n\tudata=",
		fcn,traceop(op),ptr,(long long)size);
	printudata(udata);
        fprintf(stderr,"\n");
    } else if(strcmp("free",fcn)==0) {
	void* ptr = va_arg(ap,void*);
	fprintf(stderr,"trace: %s: op=%s: ptr=%p\n\tudata=",fcn,traceop(op),ptr);
	printudata(udata);
        fprintf(stderr,"\n");
    } else if(strcmp("memcpy",fcn)==0) {
	void* dest = va_arg(ap,void*);
	void* src = va_arg(ap,void*);
	size_t size = va_arg(ap,size_t);
	fprintf(stderr,"trace: %s: op=%s: dest=%p src=%p size=%lld\n\tudata=",
		fcn,traceop(op),dest,src,(long long)size);
	printudata(udata);
        fprintf(stderr,"\n");
    } else if(strcmp("udata_copy",fcn)==0) {
#ifdef TRACE_UDATA
	fprintf(stderr,"trace: %s: udat=%p \n\tudata=",fcn,udata);
	printudata(udata);
        fprintf(stderr,"\n");
#endif
    } else if(strcmp("udata_free",fcn)==0) {
#ifdef TRACE_UDATA
	fprintf(stderr,"trace: %s: udata=%p\n\tudata=",fcn,udata);
	printudata(udata);
        fprintf(stderr,"\n");
#endif
    } else {
	fprintf(stderr,"unknown fcn: %s\n",fcn);
    }
    va_end(ap);
    fflush(stderr);
}

static void
traceend(const char* fcn, void* _udata, uintptr_t retval)
{
    H5LT_file_image_ud_t *udata = (H5LT_file_image_ud_t *)_udata;
    fprintf(stderr,"traceend: %s: retval=%p\n\tudata=",fcn,(void*)retval);
    printudata(udata);
    fprintf(stderr,"\n");
    fflush(stderr);
}

#endif /*TRACE*/

#ifdef CATCH
static void
tracefail(const char* fcn)
{
    fprintf(stderr,"fail: %s\n",fcn);
    fflush(stderr);
}
#endif /*CATCH*/

#ifdef TRACE
static void
traceflags(int flags)
{
    int i;
    for(i=0;i<16;i++) {
	if((flags & 1<<i) == 0) continue;
	switch(1<<i) {
	case H5LT_FILE_IMAGE_OPEN_RW: /* 0x0001 Open image for read-write */
	    if(i > 0) fprintf(stderr,"|");
	    fprintf(stderr,"OPEN_RW");
	    break;
	case H5LT_FILE_IMAGE_DONT_COPY: /*0x0002 the HDF5 lib won't copy   */
	    if(i > 0) fprintf(stderr,"|");
	    fprintf(stderr,"DONT_COPY");
	    break;
	case H5LT_FILE_IMAGE_DONT_RELEASE: /* 0x0004 The HDF5 lib won't
                                              deallocate user supplied image
                                              buffer. The user application
                                              is reponsible for doing so. */
	    if(i > 0) fprintf(stderr,"|");
	    fprintf(stderr,"DONT_RELEASE");
	    break;
	default: break;
	}
    }
}

static const char*
traceop(H5FD_file_image_op_t op)
{
    const char* sop = NULL;
    switch ( op ) {
    case H5FD_FILE_IMAGE_OP_NO_OP:
	sop = "NO_OP"; break;
    case H5FD_FILE_IMAGE_OP_PROPERTY_LIST_SET:
	sop = "PROPERTY_LIST_SET"; break;
    case H5FD_FILE_IMAGE_OP_PROPERTY_LIST_COPY:
	sop = "PROPERTY_LIST_COPY"; break;
    case H5FD_FILE_IMAGE_OP_PROPERTY_LIST_GET:
	sop = "PROPERTY_LIST_GET"; break;
    case H5FD_FILE_IMAGE_OP_PROPERTY_LIST_CLOSE:
	sop = "PROPERTY_LIST_CLOSE"; break;
    case H5FD_FILE_IMAGE_OP_FILE_OPEN:
	sop = "FILE_OPEN"; break;
    case H5FD_FILE_IMAGE_OP_FILE_RESIZE:
	sop = "FILE_RESIZE"; break;
    case H5FD_FILE_IMAGE_OP_FILE_CLOSE:
	sop = "FILE_CLOSE"; break;
    default: break;
    }
    return sop;
}

#endif

