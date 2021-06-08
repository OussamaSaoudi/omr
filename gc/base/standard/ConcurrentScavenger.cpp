/*******************************************************************************
 * Copyright (c) 1991, 2021 IBM Corp. and others
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at https://www.eclipse.org/legal/epl-2.0/
 * or the Apache License, Version 2.0 which accompanies this distribution and
 * is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following
 * Secondary Licenses when the conditions for such availability set
 * forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
 * General Public License, version 2 with the GNU Classpath
 * Exception [1] and GNU General Public License, version 2 with the
 * OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] http://openjdk.java.net/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/
/**
 * Perform any collector initialization particular to the concurrent collector.
 */


#include "omrcfg.h"
#include "omrcomp.h"
#include "omrmodroncore.h"
#include "mmomrhook.h"
#include "mmomrhook_internal.h"
#include "modronapicore.hpp"
#include "modronbase.h"
#include "modronopt.h"
#include "ModronAssertions.h"
#include "omr.h"
#include "thread_api.h"
#include "AllocateDescription.hpp"
#include "MemorySubSpaceSemiSpace.hpp"
#include "ConcurrentScavenger.hpp"
#include "ConcurrentScavengeTask.hpp"
#include "Scavenger.hpp"

extern "C" {
#if defined(OMR_GC_MODRON_CONCURRENT_MARK)
	void oldToOldReferenceCreated(MM_EnvironmentBase *env, omrobjectptr_t objectPtr);
#endif /* OMR_GC_MODRON_CONCURRENT_MARK */
}
/**
 * Perform any collector initialization particular to the concurrent collector.
 */
bool
MM_ConcurrentScavenger::collectorStartup(MM_GCExtensionsBase* extensions)
{
	if (_extensions->concurrentScavenger) {
		if (!_mainGCThread.startup()) {
			return false;
		}
	}
	return true;
}

/**
 * Perform any collector shutdown particular to the concurrent collector.
 * Currently this just involves stopping the concurrent background helper threads.
 */
void
MM_Scavenger::collectorShutdown(MM_GCExtensionsBase* extensions)
{
	if (_extensions->concurrentScavenger) {
		_mainGCThread.shutdown();
	}
}

/**
 * Determine whether GC stats should be calculated for this round.
 * @return true if GC stats should be calculated for this round, false otherwise.
 */
bool
MM_Scavenger::canCalcGCStats(MM_EnvironmentStandard *env)
{
	/* Only do once, at the end of cycle */
	bool canCalculate = !isConcurrentCycleInProgress();

	/* If no backout and we actually did a scavenge this time around then it's safe to gather stats */
	canCalculate &= (!isBackOutFlagRaised() && (0 < _extensions->heap->getPercolateStats()->getScavengesSincePercolate()));

	return canCalculate;
}

MMINLINE bool
MM_Scavenger::activateSurvivorCopyScanCache(MM_EnvironmentStandard *env)
{
	MM_CopyScanCacheStandard *cache = (MM_CopyScanCacheStandard *)env->_inactiveSurvivorCopyScanCache;
	if (NULL != cache) {
		Assert_MM_true(MUTATOR_THREAD == env->getThreadType());
		/* racing with a GC thread that detected work queue depletion, which may try to flush the cache to generate more concurrent work */
		if ((uintptr_t)cache == MM_AtomicOperations::lockCompareExchange((volatile uintptr_t *)&env->_inactiveSurvivorCopyScanCache, (uintptr_t)cache, (uintptr_t)NULL)) {
			/* succeded activating */
			Assert_MM_true(NULL == env->_survivorCopyScanCache);
			Assert_MM_true(0 != (cache->flags & OMR_SCAVENGER_CACHE_TYPE_CLEARED));
			cache->flags &= ~OMR_SCAVENGER_CACHE_TYPE_CLEARED;
			Assert_MM_true(env->_survivorTLHRemainderBase == cache->cacheAlloc);
			Assert_MM_true(env->_survivorTLHRemainderTop == cache->cacheTop);
			env->_survivorTLHRemainderBase = NULL;
			env->_survivorTLHRemainderTop = NULL;
			env->_survivorCopyScanCache = cache;
			activateDeferredCopyScanCache(env);
			/* Force slow path release VM access, to be able to push mutator copy caches to scanning and reliable tell if thread is inactive */
			env->forceOutOfLineVMAccess();
			return true;
		}
	}
	return false;
}


MMINLINE bool
MM_Scavenger::activateTenureCopyScanCache(MM_EnvironmentStandard *env)
{
	MM_CopyScanCacheStandard *cache = (MM_CopyScanCacheStandard *)env->_inactiveTenureCopyScanCache;
	if (NULL != cache) {
		Assert_MM_true(MUTATOR_THREAD == env->getThreadType());
		/* racing with a GC thread that detected work queue depletion, which may try to flush the cache to generate more concurrent work */
		if ((uintptr_t)cache == MM_AtomicOperations::lockCompareExchange((volatile uintptr_t *)&env->_inactiveTenureCopyScanCache, (uintptr_t)cache, (uintptr_t)NULL)) {
			/* succeded activating */
			Assert_MM_true(NULL == env->_tenureCopyScanCache);
			Assert_MM_true(0 != (cache->flags & OMR_SCAVENGER_CACHE_TYPE_CLEARED));
			cache->flags &= ~OMR_SCAVENGER_CACHE_TYPE_CLEARED;
			Assert_MM_true(env->_tenureTLHRemainderBase == cache->cacheAlloc);
			Assert_MM_true(env->_tenureTLHRemainderTop == cache->cacheTop);
			env->_tenureTLHRemainderBase = NULL;
			env->_tenureTLHRemainderTop = NULL;
			env->_tenureCopyScanCache = cache;
			activateDeferredCopyScanCache(env);
			/* Force slow path release VM access, to be able to push mutator copy caches to scanning and reliable tell if thread is inactive */
			env->forceOutOfLineVMAccess();
			return true;
		}
	}
	return false;
}

void
MM_Scavenger::activateDeferredCopyScanCache(MM_EnvironmentStandard *env)
{
	MM_CopyScanCacheStandard *cache = (MM_CopyScanCacheStandard *)env->_inactiveDeferredCopyCache;
	if (NULL != cache) {
		/* TODO: investigate if atomic us really necessary */
		if ((uintptr_t)cache == MM_AtomicOperations::lockCompareExchange((volatile uintptr_t *)&env->_inactiveDeferredCopyCache, (uintptr_t)cache, (uintptr_t)NULL)) {
			Assert_MM_true(NULL == env->_deferredCopyCache);
			env->_deferredCopyCache = cache;
		}
	}
}


MMINLINE MM_CopyScanCacheStandard *
MM_Scavenger::reserveMemoryForAllocateInSemiSpace(MM_EnvironmentStandard *env, omrobjectptr_t objectToEvacuate, uintptr_t objectReserveSizeInBytes)
{
	void* addrBase = NULL;
	void* addrTop = NULL;
	MM_CopyScanCacheStandard *copyCache = NULL;
	uintptr_t cacheSize = objectReserveSizeInBytes;

	Assert_MM_objectAligned(env, objectReserveSizeInBytes);

	/*
	 * Please note that condition like (top >= start + size) might cause wrong functioning due overflow
	 * so to be safe (top - start >= size) must be used
	 */
retry:
	if ((NULL != env->_survivorCopyScanCache) && (((uintptr_t)env->_survivorCopyScanCache->cacheTop - (uintptr_t)env->_survivorCopyScanCache->cacheAlloc) >= cacheSize)) {
		/* A survivor copy scan cache exists and there is a room, use the current copy cache */
		copyCache = env->_survivorCopyScanCache;
	} else {
		/* The copy cache was null or did not have enough room */
		/* Try and allocate room for the copy - if successful, flush the old cache */
		bool allocateResult = false;
		/* Mutator (in CS) should have VM access when copying an object (but we just check on cache refresh) */
		Assert_MM_false(env->inNative());
		if (objectReserveSizeInBytes < _minSemiSpaceFailureSize) {
			if (activateSurvivorCopyScanCache(env)) {
				goto retry;
			}
			/* try to use TLH remainder from previous discard */
			if (((uintptr_t)env->_survivorTLHRemainderTop - (uintptr_t)env->_survivorTLHRemainderBase) >= cacheSize) {
				allocateResult = true;
				addrBase = env->_survivorTLHRemainderBase;
				addrTop = env->_survivorTLHRemainderTop;
				Assert_MM_true(NULL != env->_survivorTLHRemainderBase);
				env->_survivorTLHRemainderBase = NULL;
				Assert_MM_true(NULL != env->_survivorTLHRemainderTop);
				env->_survivorTLHRemainderTop = NULL;
				activateDeferredCopyScanCache(env);
			} else if (_extensions->tlhSurvivorDiscardThreshold < cacheSize) {
				MM_AllocateDescription allocDescription(cacheSize, 0, false, true);

				addrBase = _survivorMemorySubSpace->collectorAllocate(env, this, &allocDescription);
				if(NULL != addrBase) {
					addrTop = (void *)(((uint8_t *)addrBase) + cacheSize);
					/* Check that there is no overflow */
					Assert_MM_true(addrTop >= addrBase);
					allocateResult = true;
				}
				env->_scavengerStats._semiSpaceAllocationCountLarge += 1;
			} else {
				MM_AllocateDescription allocDescription(0, 0, false, true);
				/* Update the optimum scan cache size */
				uintptr_t scanCacheSize = MM_Scavenger::calculateOptimumCopyScanCacheSize(env);
				allocateResult = (NULL != _survivorMemorySubSpace->collectorAllocateTLH(env, this, &allocDescription, scanCacheSize, addrBase, addrTop));
				env->_scavengerStats._semiSpaceAllocationCountSmall += 1;
			}
		}

		if (allocateResult) {
			/* A new chunk has been allocated - refresh the copy cache */
			if (_extensions->concurrentScavengeExhaustiveTermination && isConcurrentCycleInProgress() && (MUTATOR_THREAD == env->getThreadType())) {
				/* For CS, force slow path release VM access, to be able to push mutator copy caches to scanning and reliable tell if thread is inactive */
				env->forceOutOfLineVMAccess();
			}

			/* release local cache first. along the path we may realize that a cache structure can be re-used */
			MM_CopyScanCacheStandard *cacheToReuse = releaseLocalCopyCache(env, env->_survivorCopyScanCache);

			if (NULL == cacheToReuse) {
				/* So, we need a new cache - try to get reserved one*/
				copyCache = MM_Scavenger::getFreeCache(env);
			} else {
				copyCache = cacheToReuse;
			}

			if (NULL != copyCache) {
#if defined(OMR_SCAVENGER_TRACE)
				OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());
				omrtty_printf("{SCAV: Semispace cache allocated (%p) %p-%p}\n", copyCache, addrBase, addrTop);
#endif /* OMR_SCAVENGER_TRACE */

				/* clear all flags except "allocated in heap" might be set already*/
				copyCache->flags &= OMR_SCAVENGER_CACHE_TYPE_HEAP;
				copyCache->flags |= OMR_SCAVENGER_CACHE_TYPE_SEMISPACE | OMR_SCAVENGER_CACHE_TYPE_COPY;
				copyCache->reinitCache(addrBase, addrTop);
			} else {
				/* can not allocate a copyCache header, release allocated memory */
				/* return memory to pool */
				_survivorMemorySubSpace->abandonHeapChunk(addrBase, addrTop);
			}

			env->_survivorCopyScanCache = copyCache;
		} else {
			/* Can not allocate requested memory in survivor subspace */
			/* Record size to reduce multiple failure attempts
			 * NOTE: Since this is used across multiple threads there is a race condition between checking and setting
			 * the minimum.  This means that this value may not actually be the lowest value, or may increase.
			 */
			if (cacheSize < _minSemiSpaceFailureSize) {
				_minSemiSpaceFailureSize = cacheSize;
			}

			/* Record stats */
			env->_scavengerStats._failedFlipCount += 1;
			env->_scavengerStats._failedFlipBytes += objectReserveSizeInBytes;
		}
	}

	return copyCache;
}


/**
 * Update the given slot to point at the new location of the object, after copying
 * the object if it was not already.
 * Attempt to copy (either flip or tenure) the object and install a forwarding
 * pointer at the new location. The object may have already been copied. In
 * either case, update the slot to point at the new location of the object.
 *
 * @param objectPtrIndirect the slot to be updated
 * @return true if the new location of the object is in new space
 * @return false otherwise
 */
MMINLINE bool
MM_Scavenger::copyAndForward(MM_EnvironmentStandard *env, volatile omrobjectptr_t *objectPtrIndirect)
{
	bool toReturn = false;
	bool const compressed = _extensions->compressObjectReferences();

	/* clear effectiveCopyCache to support aliasing check -- will be updated if copy actually takes place */
	env->_effectiveCopyScanCache = NULL;

	omrobjectptr_t objectPtr = *objectPtrIndirect;
	if (NULL != objectPtr) {
		if (isObjectInEvacuateMemory(objectPtr)) {
			/* Object needs to be copy and forwarded.  Check if the work has already been done */
			MM_ForwardedHeader forwardHeader(objectPtr, compressed);
			omrobjectptr_t forwardPtr = forwardHeader.getForwardedObject();

			if (NULL != forwardPtr) {
				/* Object has been copied - update the forwarding information and return */
				toReturn = isObjectInNewSpace(forwardPtr);
				/* CS: ensure it's fully copied before exposing this new version of the object */
				forwardHeader.copyOrWait(forwardPtr);
				*objectPtrIndirect = forwardPtr;
			} else {
				omrobjectptr_t destinationObjectPtr = copy(env, &forwardHeader);
				if (NULL == destinationObjectPtr) {
					/* Failure - the scavenger must back out the work it has done. */
					/* raise the alert and return (true - must look like a new object was handled) */
					toReturn = true;
					if (_extensions->concurrentScavenger) {
						/* We have no place to copy. We will return the original location of the object.
						 * But we must prevent any other thread of making a copy of this object.
						 * So we will attempt to atomically self forward it.  */
						forwardPtr = forwardHeader.setSelfForwardedObject();
						if (forwardPtr != objectPtr) {
							/* Failed to self-forward (someone successfully copied it). Re-fetch the forwarding info
							 * and ensure it's fully copied before exposing this new version of the object */
							toReturn = isObjectInNewSpace(forwardPtr);
							MM_ForwardedHeader(objectPtr, compressed).copyOrWait(forwardPtr);
							*objectPtrIndirect = forwardPtr;
						}
					}
				} else {
					/* Update the slot. copy() ensures the object is fully copied */
					toReturn = isObjectInNewSpace(destinationObjectPtr);
					*objectPtrIndirect = destinationObjectPtr;
				}
			}
		} else if (isObjectInNewSpace(objectPtr)) {
#if defined(OMR_GC_MODRON_SCAVENGER_STRICT)
			MM_ForwardedHeader forwardHeader(objectPtr, compressed);
			Assert_MM_true(!forwardHeader.isForwardedPointer());
#endif /* defined(OMR_GC_MODRON_SCAVENGER_STRICT) */
			/* When slot has been scanned before, and is already copied or forwarded
			 * for example when the partial scan state of a cache has been lost in scan cache overflow
			 */
			toReturn = true;
#if defined(OMR_GC_MODRON_SCAVENGER_STRICT)
		} else {
			Assert_MM_true(_extensions->isOld(objectPtr));
#endif /* defined(OMR_GC_MODRON_SCAVENGER_STRICT) */
		}
	}

	return toReturn;
}


/**
 * Update the given slot to point at the new location of the object, after copying
 * the object if it was not already.
 * Attempt to copy (either flip or tenure) the object and install a forwarding
 * pointer at the new location. The object may have already been copied. In
 * either case, update the slot to point at the new location of the object.
 *
 * @param slotObject the slot to be updated
 * @return true if the new location of the object is in new space
 * @return false otherwise
 */
MMINLINE bool
MM_Scavenger::copyAndForward(MM_EnvironmentStandard *env, GC_SlotObject *slotObject)
{
	omrobjectptr_t oldSlot = slotObject->readReferenceFromSlot();
	omrobjectptr_t slot = oldSlot;
	bool result = copyAndForward(env, &slot);
	if (concurrent_phase_scan == _concurrentPhase) {
		if (oldSlot != slot) {
			slotObject->atomicWriteReferenceToSlot(oldSlot, slot);
		}
	} else
	{
		slotObject->writeReferenceToSlot(slot);
	}
#if defined(OMR_SCAVENGER_TRACK_COPY_DISTANCE)
	if (NULL != env->_effectiveCopyScanCache) {
		env->_scavengerStats.countCopyDistance((uintptr_t)slotObject->readAddressFromSlot(), (uintptr_t)slotObject->readReferenceFromSlot());
	}
#endif /* OMR_SCAVENGER_TRACK_COPY_DISTANCE */
	return result;
}

omrobjectptr_t
MM_Scavenger::copyObject(MM_EnvironmentStandard *env, MM_ForwardedHeader* forwardedHeader)
{
#if defined(OMR_GC_CONCURRENT_SCAVENGER)
	/* todo: find an elegant way to force abort triggered by non GC threads */
//	if (0 == (uint64_t)forwardedHeader->getObject() % 27449) {
//		setBackOutFlag(env, backOutFlagRaised);
//		return NULL;
//	}
#endif /* OMR_GC_CONCURRENT_SCAVENGER */
	return copy(env, forwardedHeader);
}


omrobjectptr_t
MM_Scavenger::copy(MM_EnvironmentStandard *env, MM_ForwardedHeader* forwardedHeader)
{
	uintptr_t objectCopySizeInBytes, objectReserveSizeInBytes;
	uintptr_t hotFieldsDescriptor = 0;
	uintptr_t hotFieldsAlignment = 0;
	uintptr_t* hotFieldPadBase = NULL;
	uintptr_t hotFieldPadSize = 0;
	MM_CopyScanCacheStandard *copyCache = NULL;
	bool const compressed = _extensions->compressObjectReferences();

	if (isBackOutFlagRaised()) {
		/* Waste of time to copy, if we aborted */
		return NULL;
	}
	/* Try and find memory for the object based on its age */
	uintptr_t objectAge = _extensions->objectModel.getPreservedAge(forwardedHeader);
	uintptr_t oldObjectAge = objectAge;

	/* Object is in the evacuate space but not forwarded. */
	_extensions->objectModel.calculateObjectDetailsForCopy(env, forwardedHeader, &objectCopySizeInBytes, &objectReserveSizeInBytes, &hotFieldsDescriptor);

	Assert_MM_objectAligned(env, objectReserveSizeInBytes);

	if (0 == (((uintptr_t)1 << objectAge) & _tenureMask)) {
		/* The object should be flipped - try to reserve room in the semi space */
		copyCache = reserveMemoryForAllocateInSemiSpace(env, forwardedHeader->getObject(), objectReserveSizeInBytes);
		if (NULL != copyCache) {
			/* Adjust the age value*/
			if(objectAge < OBJECT_HEADER_AGE_MAX) {
				objectAge += 1;
			}
		} else {
			Trc_MM_Scavenger_semispaceAllocateFailed(env->getLanguageVMThread(), objectReserveSizeInBytes, "yes");

			uintptr_t spaceAvailableForObject = _activeSubSpace->getMaxSpaceForObjectInEvacuateMemory(forwardedHeader->getObject());
			Assert_GC_true_with_message4(env, objectCopySizeInBytes <= spaceAvailableForObject,
					"Corruption in Evacuate at %p: calculated object size %zu larger then available %zu, Forwarded Header at %p\n",
					forwardedHeader->getObject(), objectCopySizeInBytes, spaceAvailableForObject, forwardedHeader);

			copyCache = reserveMemoryForAllocateInTenureSpace(env, forwardedHeader->getObject(), objectReserveSizeInBytes);
			if (NULL != copyCache) {
				/* Clear age and set the old bit */
				objectAge = STATE_NOT_REMEMBERED;
			} else {
				Trc_MM_Scavenger_tenureAllocateFailed(env->getLanguageVMThread(), objectReserveSizeInBytes, env->_scavengerStats._failedTenureLargest, "no");
			}
		}
	} else {
		/* Move straight to tenuring on the object */
		/* adjust the reserved object's size if we are aligning hot fields and this class has a known hot field */
		if (_extensions->scavengerAlignHotFields && HOTFIELD_SHOULD_ALIGN(hotFieldsDescriptor)) {
			/* this optimization is a source of fragmentation (alloc request size always assumes maximum padding,
			 * but free entry created by sweep in tenure could be less than that (since some of unused padding can overlap with next copied object)).
			 * we limit this optimization for arrays up to the size of 2 cache lines, beyond which the benefits of the optimization are believed to be non-existant */
            if (!_extensions->objectModel.isIndexable(forwardedHeader) || (objectReserveSizeInBytes <= 2 * _cacheLineAlignment)) {
				/* set the descriptor field if we should be aligning (since assuming that 0 means no is not safe) */
				hotFieldsAlignment = hotFieldsDescriptor;
				/* for simplicity, add the maximum padding we could need (and back off after allocation) */
				objectReserveSizeInBytes += (_cacheLineAlignment - _objectAlignmentInBytes);
				Assert_MM_objectAligned(env, objectReserveSizeInBytes);
            }
		}
		copyCache = reserveMemoryForAllocateInTenureSpace(env, forwardedHeader->getObject(), objectReserveSizeInBytes);
		if (NULL != copyCache) {
			/* Clear age and set the old bit */
			objectAge = STATE_NOT_REMEMBERED;
		} else {
			Trc_MM_Scavenger_tenureAllocateFailed(env->getLanguageVMThread(), objectReserveSizeInBytes, env->_scavengerStats._failedTenureLargest, "yes");

			uintptr_t spaceAvailableForObject = _activeSubSpace->getMaxSpaceForObjectInEvacuateMemory(forwardedHeader->getObject());
			Assert_GC_true_with_message4(env, objectCopySizeInBytes <= spaceAvailableForObject,
					"Corruption in Evacuate at %p: calculated object size %zu larger then available %zu, Forwarded Header at %p\n",
					forwardedHeader->getObject(), objectCopySizeInBytes, spaceAvailableForObject, forwardedHeader);

			copyCache = reserveMemoryForAllocateInSemiSpace(env, forwardedHeader->getObject(), objectReserveSizeInBytes);
			if (NULL != copyCache) {
				/* Adjust the age value*/
				if(objectAge < OBJECT_HEADER_AGE_MAX) {
					objectAge += 1;
				} else {
					Trc_MM_Scavenger_semispaceAllocateFailed(env->getLanguageVMThread(), objectReserveSizeInBytes, "no");
				}
			}
		}
	}

	/* Check if memory was reserved successfully */
	if (NULL == copyCache) {
		/* Failure - the scavenger must back out the work it has done. */
		/* raise the alert and return (with NULL) */
		setBackOutFlag(env, backOutFlagRaised);
		omrthread_monitor_enter(_scanCacheMonitor);
		if (0 != _waitingCount) {
			omrthread_monitor_notify_all(_scanCacheMonitor);
		}
		omrthread_monitor_exit(_scanCacheMonitor);
		return NULL;
	}

	/* Memory has been reserved */
	omrobjectptr_t destinationObjectPtr = (omrobjectptr_t)copyCache->cacheAlloc;
	/* now correct for the hot field alignment */
	if (0 != hotFieldsAlignment) {
		uintptr_t remainingInCacheLine = _cacheLineAlignment - ((uintptr_t)destinationObjectPtr % _cacheLineAlignment);
		uintptr_t alignmentBias = HOTFIELD_ALIGNMENT_BIAS(hotFieldsAlignment, _objectAlignmentInBytes);
		/* do alignment only if the object cannot fit in the remaining space in the cache line */
		if ((remainingInCacheLine < objectCopySizeInBytes) && (alignmentBias < remainingInCacheLine)) {
			hotFieldPadSize = ((remainingInCacheLine + _cacheLineAlignment) - (alignmentBias % _cacheLineAlignment)) % _cacheLineAlignment;
			hotFieldPadBase = (uintptr_t *)destinationObjectPtr;
			/* now fix the object pointer so that the hot field is aligned */
			destinationObjectPtr = (omrobjectptr_t)((uintptr_t)destinationObjectPtr + hotFieldPadSize);
		}
		/* and update the reserved size so that we "un-reserve" the extra memory we said we might need.  This is done by
		 * removing the excess reserve since we already accounted for the hotFieldPadSize by bumping the destination pointer
		 * and now we need to revert to the amount needed for the object allocation and its array alignment so the rest of
		 * the method continues to function without needing to know about this extra alignment calculation
		 */
		objectReserveSizeInBytes = objectReserveSizeInBytes - (_cacheLineAlignment - _objectAlignmentInBytes);
	}

	/* and correct for the double array alignment */
	void *newCacheAlloc = (void *) (((uint8_t *)destinationObjectPtr) + objectReserveSizeInBytes);

	omrobjectptr_t originalDestinationObjectPtr = destinationObjectPtr;
	uintptr_t remainingSizeToCopy = 0;
	uintptr_t initialSizeToCopy = 0;
	bool allowDuplicate = false;
	bool allowDuplicateOrConcurrentDisabled = true;

	if (IS_CONCURRENT_ENABLED) {
		/* For smaller objects, we allow duplicate (copy first and try to win forwarding).
		 * For larger objects, there is only one copy (threads setup destination header, one wins, and other participate in copying or wait till copy is complete).
		 * 1024 is somewhat arbitrary threshold, so that most of time we do not have to go through relatively expensive setup procedure.
		 */
		if (objectCopySizeInBytes <= 1024) {
			allowDuplicate = true;
		} else {
			remainingSizeToCopy = objectCopySizeInBytes;
			initialSizeToCopy = forwardedHeader->copySetup(destinationObjectPtr, &remainingSizeToCopy);
			/* set the hint in the f/w pointer, that the object might still be in the processes of copying */
			destinationObjectPtr = forwardedHeader->setForwardedObjectWithBeingCopiedHint(destinationObjectPtr);
			allowDuplicateOrConcurrentDisabled = false;
		}
	} else {
		destinationObjectPtr = forwardedHeader->setForwardedObject(destinationObjectPtr);
	}

	/* outter if-forwarding-succeeded check */
	if (originalDestinationObjectPtr == destinationObjectPtr) {
		/* Succeeded in forwarding the object [nonCS],
		 * or we allow duplicate (did not even tried to forward yet) [CS].
		 */

		if (NULL != hotFieldPadBase) {
			/* lay down a hole (XXX:  This assumes that we are using AOL (address-ordered-list)) */
			MM_HeapLinkedFreeHeader::fillWithHoles(hotFieldPadBase, hotFieldPadSize, compressed);
		}

#if defined(OMR_VALGRIND_MEMCHECK)
		valgrindMempoolAlloc(_extensions, (uintptr_t) destinationObjectPtr, objectReserveSizeInBytes);
#endif /* defined(OMR_VALGRIND_MEMCHECK) */

		if (!allowDuplicateOrConcurrentDisabled) {
			/* Copy a non-aligned section */
			forwardedHeader->copySection(destinationObjectPtr, remainingSizeToCopy, initialSizeToCopy);

			/* Try to copy more aligned sections. Once no more sections to copy, wait till other threads are done with their sections */
			forwardedHeader->copyOrWaitWinner(destinationObjectPtr);

			/* Fixup most of the destination object (part that overlaps with forwarded header) */
			forwardedHeader->commenceFixup(destinationObjectPtr);

			/* Object model specific fixup, like age */
			_extensions->objectModel.fixupForwardedObject(forwardedHeader, destinationObjectPtr, objectAge);

			/* Final fixup step - the object is available for usage by mutator threads */
			forwardedHeader->commitFixup(destinationObjectPtr);
		} else
		{
			memcpy((void *)destinationObjectPtr, forwardedHeader->getObject(), objectCopySizeInBytes);

			/* Copy the preserved fields from the forwarded header into the destination object */
			forwardedHeader->fixupForwardedObject(destinationObjectPtr);

			_extensions->objectModel.fixupForwardedObject(forwardedHeader, destinationObjectPtr, objectAge);
		}

#if defined(OMR_VALGRIND_MEMCHECK)
		valgrindFreeObject(_extensions,(uintptr_t) forwardedHeader->getObject());

		// Object is definitely dead but at many places (glue : ScavangerRootScanner)
		// We use it's forwardedHeader to check it.
		valgrindMakeMemDefined((uintptr_t) forwardedHeader->getObject(), sizeof(MM_ForwardedHeader));

#endif /* defined(OMR_VALGRIND_MEMCHECK) */

#if defined(OMR_SCAVENGER_TRACE_COPY)
		OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());
		omrtty_printf("{SCAV: Copied %p[%p] -> %p[%p]}\n", forwardedHeader->getObject(), *((uintptr_t*)(forwardedHeader->getObject())), destinationObjectPtr, *((uintptr_t*)destinationObjectPtr));
#endif /* OMR_SCAVENGER_TRACE_COPY */

		/* Concurrent Scavenger can update forwarding pointer only after the object has been copied
		 * (since mutator may access the object as soon as forwarding pointer is installed) */
		if (allowDuplicate) {
			/* On weak memory model, ensure that this candidate copy is visible
			 * before (potentially) winning forwarding */
			MM_AtomicOperations::storeSync();
			destinationObjectPtr = forwardedHeader->setForwardedObject(destinationObjectPtr);
		}

		/* nested if-forwarding-succeeded check */
		if (originalDestinationObjectPtr == destinationObjectPtr) {
			/* Succeeded in forwarding the object */
			forwardingSucceeded(env, copyCache, newCacheAlloc, oldObjectAge, objectCopySizeInBytes, objectReserveSizeInBytes);

			/* depth copy the hot fields of an object if scavenger dynamicBreadthFirstScanOrdering is enabled */
			depthCopyHotFields(env, forwardedHeader, destinationObjectPtr);
		} else { /* CS build flag  enabled: mid point of nested if-forwarding-succeeded check */

			forwardingFailed(env, forwardedHeader, destinationObjectPtr, copyCache);

		} /* CS build flag  enabled: end of nested if-forwarding-succeeded check */
	} else { /* CS build flag  enabled: mid point of outter   if-forwarding-succeeded check
	          * CS build flag disabled: mid point of the only if-forwarding-succeeded check */

		forwardingFailed(env, forwardedHeader, destinationObjectPtr, copyCache);

	} /* CS build flag  enabled: end of outter   if-forwarding-succeeded check
	   * CS build flag disabled: end of the only if-forwarding-succeeded check */

	/* return value for updating the slot */
	return destinationObjectPtr;
}

/****************************************
 * Scan completion routines
 ****************************************
 */

void
MM_Scavenger::externalNotifyToYield(MM_EnvironmentBase *env)
{
	if (isCurrentPhaseConcurrent()) {
		omrthread_monitor_enter(_scanCacheMonitor);
		_shouldYield = true;
		if (0 != _waitingCount) {
			omrthread_monitor_notify_all(_scanCacheMonitor);
		}
		omrthread_monitor_exit(_scanCacheMonitor);
	}
}

MMINLINE void
MM_Scavenger::flushBuffersForGetNextScanCache(MM_EnvironmentStandard *env, bool finalFlush)
{
	_delegate.flushReferenceObjects(env);
	flushRememberedSet(env);
	MM_Scavenger::flushCopyScanCounts(env, finalFlush);

	if (_extensions->concurrentScavengeExhaustiveTermination && isCurrentPhaseConcurrent()) {
		/* need to return empty caches to the global pool so they can be accurately counted when evaluating 'all caches returned' termination criteria */
		returnEmptyCopyCachesToFreeList(env);
	}
}


bool
MM_Scavenger::shouldDoFinalNotify(MM_EnvironmentStandard *env)
{
	if (_extensions->concurrentScavengeExhaustiveTermination && isCurrentPhaseConcurrent() && !_scavengeCacheFreeList.areAllCachesReturned()) {

		/* GC threads ran out of work, but not all copy caches are back to the global free pool. They are kept by mutator threads.
		 * Activate Async Signal handler which will force Mutator threads to flush their copy caches for scanning. */
		_delegate.signalThreadsToFlushCaches(env);

		/* If no work has been created and no one requested to yeild meanwhile, go and wait for new work */
		if (!checkAndSetShouldYieldFlag(env)) {
			if (0 == _cachedEntryCount) {
				Assert_MM_true(!_scavengeCacheFreeList.areAllCachesReturned());

				/* The only known reason for timeout is a rare case if Exclusive VM Access request came from a nonGC party. If we did not have a timeout,
				 * we would end up wating for mutator threads that hold Copy Caches, but they would not respond if they already released VM access
				 * and blocked due to ongoing Exclusive, hence creating deadlock. Timeout of 1ms gives a chance to check if we should yield and release VM access.
				 * Alternative solutions to providing timeout to consider in future:
				 * 1) notify (via hook) GC that Exclusive is requested (proven to work, but breaks general async nature of how Exclusive Request is requested)
				 * 2) release VM access prior to blocking (tricky since thread that blocks is not necessarily Main, which is the one that holds VM access)
				 */
				omrthread_monitor_wait_timed(_scanCacheMonitor, 1, 0);
			}
			/* We know there is more work - can't do the final notify yet. Need to help with work and eventually re-evaulate if it's really the end */
			return false;
		}
		/* If we have to yield, we do need to notify worker threads to unblock and temporarily completely scan loop */
	}
	return true;
}


void
MM_Scavenger::pruneRememberedSetList(MM_EnvironmentStandard *env)
{
	/* Remembered set walk */
	omrobjectptr_t *slotPtr;
	omrobjectptr_t objectPtr;
	MM_SublistPuddle *puddle;

#if defined(OMR_SCAVENGER_TRACE_REMEMBERED_SET)
	OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());
	omrtty_printf("{SCAV: Begin prune remembered set list; count = %lld}\n", _extensions->rememberedSet.countElements());
#endif /* OMR_SCAVENGER_TRACE_REMEMBERED_SET */

	GC_SublistIterator remSetIterator(&(_extensions->rememberedSet));
	while((puddle = remSetIterator.nextList()) != NULL) {
		if(J9MODRON_HANDLE_NEXT_WORK_UNIT(env)) {
			GC_SublistSlotIterator remSetSlotIterator(puddle);
			while((slotPtr = (omrobjectptr_t *)remSetSlotIterator.nextSlot()) != NULL) {
				objectPtr = *slotPtr;

				if (NULL == objectPtr) {
					remSetSlotIterator.removeSlot();
				} else if((uintptr_t)objectPtr & DEFERRED_RS_REMOVE_FLAG) {
					/* Is slot flagged for deferred removal ? */
					/* Yes..so first remove tag bit from object address */
					objectPtr = (omrobjectptr_t)((uintptr_t)objectPtr & ~(uintptr_t)DEFERRED_RS_REMOVE_FLAG);
					/* The object did not have Nursery references at initial RS scan, but one could have been added during CS cycle by a mutator. */
					if (!IS_CONCURRENT_ENABLED || !shouldRememberObject(env, objectPtr)) {
#if defined(OMR_SCAVENGER_TRACE_REMEMBERED_SET)
						omrtty_printf("{SCAV: REMOVED remembered set object %p}\n", objectPtr);
#endif /* OMR_SCAVENGER_TRACE_REMEMBERED_SET */

						/* A simple mask out can be used - we are guaranteed to be the only manipulator of the object */
						_extensions->objectModel.clearRemembered(objectPtr);
						remSetSlotIterator.removeSlot();
						/* Inform interested parties (Concurrent Marker) that an object has been removed from the remembered set.
						 * In non-concurrent Scavenger this is the only way to create an old-to-old reference, that has parent object being marked.
						 * In Concurrent Scavenger, it can be created even with parent object that was not in RS to start with. So this is handled
						 * in a more generic spot when object is scavenged and is unnecessary to do it here.
						 */
#if defined(OMR_GC_MODRON_CONCURRENT_MARK)
						if (_extensions->shouldScavengeNotifyGlobalGCOfOldToOldReference() && !IS_CONCURRENT_ENABLED) {
							oldToOldReferenceCreated(env, objectPtr);
						}
#endif /* OMR_GC_MODRON_CONCURRENT_MARK */
					} else {
						/* We are not removing it after all, since the object has Nursery references => reset the deferred flag.
						 * todo: consider doing double remembering, if remembered during CS cycle, to avoid the rescan of the object
						 */
						*slotPtr = objectPtr;
					}

				} else {
					/* Retain remembered object */
#if defined(OMR_SCAVENGER_TRACE_REMEMBERED_SET)
					omrtty_printf("{SCAV: Remembered set object %p}\n", objectPtr);
#endif /* OMR_SCAVENGER_TRACE_REMEMBERED_SET */

					if (!IS_CONCURRENT_ENABLED && processRememberedThreadReference(env, objectPtr)) {
						/* the object was tenured from the stack on a previous scavenge -- keep it around for a bit longer */
						Trc_MM_ParallelScavenger_scavengeRememberedSet_keepingRememberedObject(env->getLanguageVMThread(), objectPtr, _extensions->objectModel.getRememberedBits(objectPtr));
					}
				}
			} /* while non-null slots */
		}
	}
#if defined(OMR_SCAVENGER_TRACE_REMEMBERED_SET)
	omrtty_printf("{SCAV: End prune remembered set list; count = %lld}\n", _extensions->rememberedSet.countElements());
#endif /* OMR_SCAVENGER_TRACE_REMEMBERED_SET */
}


void
MM_Scavenger::scavengeRememberedSetListDirect(MM_EnvironmentStandard *env)
{
	Trc_MM_ParallelScavenger_scavengeRememberedSetList_Entry(env->getLanguageVMThread());

	MM_SublistPuddle *puddle = NULL;
	while (NULL != (puddle = _extensions->rememberedSet.popPreviousPuddle(puddle))) {
		Trc_MM_ParallelScavenger_scavengeRememberedSetList_startPuddle(env->getLanguageVMThread(), puddle);
		uintptr_t numElements = 0;
		GC_SublistSlotIterator remSetSlotIterator(puddle);
		omrobjectptr_t *slotPtr;
		while((slotPtr = (omrobjectptr_t *)remSetSlotIterator.nextSlot()) != NULL) {
			omrobjectptr_t objectPtr = *slotPtr;

			/* Ignore flaged for removal by the indirect refs pass */
			if (0 == ((uintptr_t)objectPtr & DEFERRED_RS_REMOVE_FLAG)) {
				if (!_extensions->objectModel.hasIndirectObjectReferents((CLI_THREAD_TYPE*)env->getLanguageVMThread(), objectPtr)) {
					Assert_MM_true(_extensions->objectModel.isRemembered(objectPtr));
					numElements += 1;
					*slotPtr = (omrobjectptr_t)((uintptr_t)objectPtr | DEFERRED_RS_REMOVE_FLAG);
					bool shouldBeRemembered = scavengeObjectSlots(env, NULL, objectPtr, GC_ObjectScanner::scanRoots, slotPtr);
					if (shouldBeRemembered) {
						/* We want to remember this object after all; clear the flag for removal. */
						*slotPtr = objectPtr;
					}
				}
			}
		}

		Trc_MM_ParallelScavenger_scavengeRememberedSetList_donePuddle(env->getLanguageVMThread(), puddle, numElements);
	}

	Trc_MM_ParallelScavenger_scavengeRememberedSetList_Exit(env->getLanguageVMThread());
}
#if 0
void
MM_Scavenger::scavengeRememberedSetListIndirect(MM_EnvironmentStandard *env)
{
	Trc_MM_ParallelScavenger_scavengeRememberedSetList_Entry(env->getLanguageVMThread());

	MM_SublistPuddle *puddle = NULL;
	while (NULL != (puddle = _extensions->rememberedSet.popPreviousPuddle(puddle))) {
		Trc_MM_ParallelScavenger_scavengeRememberedSetList_startPuddle(env->getLanguageVMThread(), puddle);
		uintptr_t numElements = 0;
		GC_SublistSlotIterator remSetSlotIterator(puddle);
		omrobjectptr_t *slotPtr;
		while((slotPtr = (omrobjectptr_t *)remSetSlotIterator.nextSlot()) != NULL) {
			omrobjectptr_t objectPtr = *slotPtr;

			if(NULL != objectPtr) {
				if (_extensions->objectModel.hasIndirectObjectReferents((CLI_THREAD_TYPE*)env->getLanguageVMThread(), objectPtr)) {
					numElements += 1;
					Assert_MM_true(_extensions->objectModel.isRemembered(objectPtr));
					*slotPtr = (omrobjectptr_t)((uintptr_t)objectPtr | DEFERRED_RS_REMOVE_FLAG);
					bool shouldBeRemembered = _delegate.scavengeIndirectObjectSlots(env, objectPtr);
					shouldBeRemembered |= scavengeObjectSlots(env, NULL, objectPtr, GC_ObjectScanner::scanRoots, slotPtr);
					if (shouldBeRemembered) {
						/* We want to remember this object after all; clear the flag for removal. */
						*slotPtr = objectPtr;
					}
				}
			} else {
				remSetSlotIterator.removeSlot();
			}
		}

		Trc_MM_ParallelScavenger_scavengeRememberedSetList_donePuddle(env->getLanguageVMThread(), puddle, numElements);
	}

	Trc_MM_ParallelScavenger_scavengeRememberedSetList_Exit(env->getLanguageVMThread());
}


/* NOTE - only  scavengeRememberedSetOverflow ends with a sync point.
 * Callers of this function must not assume that there is a sync point
 */
void
MM_Scavenger::scavengeRememberedSet(MM_EnvironmentStandard *env)
{
	if (_isRememberedSetInOverflowAtTheBeginning) {
		env->_scavengerStats._rememberedSetOverflow = 1;
		/* For CS, in case of OF, we deal with both direct and indirect refs with only one pass. */
		if (!IS_CONCURRENT_ENABLED || (concurrent_phase_roots == _concurrentPhase)) {
			scavengeRememberedSetOverflow(env);
		}
	} else {
		if (!IS_CONCURRENT_ENABLED) {
			scavengeRememberedSetList(env);
		}
		/* Indirect refs are dealt within the root scanning phase (first STW phase), while the direct references are dealt within the main scan phase (typically concurrent). */
		else if (concurrent_phase_roots == _concurrentPhase) {
			scavengeRememberedSetListIndirect(env);
		} else if (concurrent_phase_scan == _concurrentPhase) {
			scavengeRememberedSetListDirect(env);
		} else {
			Assert_MM_unreachable();
		}
	}
}


MMINLINE bool
MM_Scavenger::checkAndSetShouldYieldFlag(MM_EnvironmentStandard *env) {
	/* Don't rely on various conditions being same during one concurrent phase.
	 * If one GC thread decided that we need to yield, we must yield, there is no way back. Hence, we store the result in _shouldYield,
	 * and rely on it for the rest of concurrent phase.
	 * Main info if we should yield comes from exclusive VM access request being broadcasted to this thread (isExclusiveAccessRequestWaiting())
	 * But since that request in the thread is not cleared even when implicit main GC thread enters STW phase, and since this yield check is invoked
	 * in common code that can run both during STW and concurrent phase, we have to additionally check we are indeed in concurrent phase before deciding to yield.
	 */

	if (isCurrentPhaseConcurrent() && env->isExclusiveAccessRequestWaiting() && !_shouldYield) {
		/* If we are yielding we must be working concurrently, so GC better not have exclusive VM access. We can really only assert it for the current thread */
		Assert_MM_true(0 == env->getOmrVMThread()->exclusiveCount);
		_shouldYield = true;
	}
	return _shouldYield;

	return false;

}

void
MM_Scavenger::backoutFixupAndReverseForwardPointersInSurvivor(MM_EnvironmentStandard *env)
{
	GC_MemorySubSpaceRegionIteratorStandard evacuateRegionIterator(_activeSubSpace);
	MM_HeapRegionDescriptorStandard* rootRegion = NULL;
	bool const compressed = _extensions->compressObjectReferences();

#if defined(OMR_SCAVENGER_TRACE_BACKOUT)
	OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());
#endif /* OMR_SCAVENGER_TRACE_BACKOUT */
	while(NULL != (rootRegion = evacuateRegionIterator.nextRegion())) {
		/* skip survivor regions */
		if (isObjectInEvacuateMemory((omrobjectptr_t )rootRegion->getLowAddress())) {
			/* tell the object iterator to work on the given region */
			GC_ObjectHeapIteratorAddressOrderedList evacuateHeapIterator(_extensions, rootRegion, false);
			evacuateHeapIterator.includeForwardedObjects();

			omrobjectptr_t objectPtr = NULL;

#if defined(OMR_SCAVENGER_TRACE_BACKOUT)
			omrtty_printf("{SCAV: Back out forward pointers in region [%p:%p]}\n", rootRegion->getLowAddress(), rootRegion->getHighAddress());
#endif /* OMR_SCAVENGER_TRACE_BACKOUT */

			while((objectPtr = evacuateHeapIterator.nextObjectNoAdvance()) != NULL) {
				MM_ForwardedHeader header(objectPtr, compressed);
				if (header.isForwardedPointer()) {
					omrobjectptr_t forwardedObject = header.getForwardedObject();
					omrobjectptr_t originalObject = header.getObject();

					_delegate.reverseForwardedObject(env, &header);

					/* A reverse forwarded object is a hole whose 'next' pointer actually points at the original object.
					 * This keeps tenure space walkable once the reverse forwarded objects are abandoned.
					 */
					UDATA evacuateObjectSizeInBytes = _extensions->objectModel.getConsumedSizeInBytesWithHeader(forwardedObject);
					MM_HeapLinkedFreeHeader* freeHeader = MM_HeapLinkedFreeHeader::getHeapLinkedFreeHeader(forwardedObject);
#if defined(OMR_VALGRIND_MEMCHECK)
					valgrindMempoolAlloc(_extensions,(uintptr_t) originalObject, (uintptr_t) evacuateObjectSizeInBytes);
					valgrindFreeObject(_extensions, (uintptr_t) forwardedObject);
					valgrindMakeMemUndefined((uintptr_t)freeHeader, (uintptr_t) sizeof(MM_HeapLinkedFreeHeader));
#endif /* defined(OMR_VALGRIND_MEMCHECK) */
					freeHeader->setNext((MM_HeapLinkedFreeHeader*)originalObject, compressed);
					freeHeader->setSize(evacuateObjectSizeInBytes);
#if defined(OMR_SCAVENGER_TRACE_BACKOUT)
					omrtty_printf("{SCAV: Back out forward pointer %p[%p]@%p -> %p[%p]}\n", objectPtr, *objectPtr, forwardedObject, freeHeader->getNext(env), freeHeader->getSize());
					Assert_MM_true(objectPtr == originalObject);
#endif /* OMR_SCAVENGER_TRACE_BACKOUT */
				}
			}
		}
	}

#if defined (OMR_GC_COMPRESSED_POINTERS)
	if (compressed) {
		GC_MemorySubSpaceRegionIteratorStandard evacuateRegionIterator1(_activeSubSpace);
		while(NULL != (rootRegion = evacuateRegionIterator1.nextRegion())) {
			if (isObjectInEvacuateMemory((omrobjectptr_t )rootRegion->getLowAddress())) {
				/*
				 * CMVC 179190:
				 * The call to "reverseForwardedObject", above, destroys our ability to detect if this object needs its destroyed slot fixed up (but
				 * the above loop must complete before we have the information with which to fixup the destroyed slot).  Fixing up a slot in dark
				 * matter could crash, though, since the slot could point to contracted memory or could point to corrupted data updated in a previous
				 * backout.  The simple work-around for this problem is to check if the slot points at a readable part of the heap (specifically,
				 * tenure or survivor - the only locations which would require us to fix up the slot) and only read and fixup the slot in those cases.
				 * This means that we could still corrupt the slot but we will never crash during fixup and nobody else should be trusting slots found
				 * in dead objects.
				 */
				GC_ObjectHeapIteratorAddressOrderedList evacuateHeapIterator(_extensions, rootRegion, false);
				omrobjectptr_t objectPtr = NULL;

				while((objectPtr = evacuateHeapIterator.nextObjectNoAdvance()) != NULL) {
					MM_ForwardedHeader header(objectPtr, compressed);
#if defined(OMR_SCAVENGER_TRACE_BACKOUT)
					uint32_t originalOverlap = header.getPreservedOverlap();
#endif /* OMR_SCAVENGER_TRACE_BACKOUT */
_delegate.fixupDestroyedSlot(env, &header, _activeSubSpace);
#if defined(OMR_SCAVENGER_TRACE_BACKOUT)
					omrobjectptr_t fwdObjectPtr = header.getForwardedObject();
					omrtty_printf("{SCAV: Fixup destroyed slot %p@%p -> %u->%u}\n", objectPtr, fwdObjectPtr, originalOverlap, header.getPreservedOverlap());
#endif /* OMR_SCAVENGER_TRACE_BACKOUT */
				}
			}
		}
	}
#endif /* defined (OMR_GC_COMPRESSED_POINTERS) */
}

bool
MM_Scavenger::fixupSlotWithoutCompression(volatile omrobjectptr_t *slotPtr)
{
	omrobjectptr_t objectPtr = *slotPtr;
	bool const compressed = _extensions->compressObjectReferences();

	if(NULL != objectPtr) {
		MM_ForwardedHeader forwardHeader(objectPtr, compressed);
		omrobjectptr_t forwardPtr = forwardHeader.getNonStrictForwardedObject();
		if (NULL != forwardPtr) {
			if (forwardHeader.isSelfForwardedPointer()) {
				forwardHeader.restoreSelfForwardedPointer();
			} else {
				*slotPtr = forwardPtr;
			}
			return true;
		}
	}
	return false;
}

bool
MM_Scavenger::fixupSlot(GC_SlotObject *slotObject)
{
	omrobjectptr_t objectPtr = slotObject->readReferenceFromSlot();
	bool const compressed = _extensions->compressObjectReferences();

	if(NULL != objectPtr) {
		MM_ForwardedHeader forwardHeader(objectPtr, compressed);
		if (forwardHeader.isStrictlyForwardedPointer()) {
			slotObject->writeReferenceToSlot(forwardHeader.getForwardedObject());
			Assert_MM_false(isObjectInEvacuateMemory(slotObject->readReferenceFromSlot()));
			return true;
		} else {
			Assert_MM_false(_extensions->objectModel.isDeadObject(objectPtr));
		}
	}
	return false;
}

void
MM_Scavenger::fixupObjectScan(MM_EnvironmentStandard *env, omrobjectptr_t objectPtr)
{
	GC_SlotObject *slotObject = NULL;
	GC_ObjectScannerState objectScannerState;
	GC_ObjectScanner *objectScanner = getObjectScanner(env, objectPtr, (void *) &objectScannerState, GC_ObjectScanner::scanRoots);
	if (NULL != objectScanner) {
		while (NULL != (slotObject = objectScanner->getNextSlot())) {
			fixupSlot(slotObject);
		}
	}

	if (_extensions->objectModel.hasIndirectObjectReferents((CLI_THREAD_TYPE*)env->getLanguageVMThread(), objectPtr)) {
		_delegate.fixupIndirectObjectSlots(env, objectPtr);
	}
}


void
MM_Scavenger::processRememberedSetInBackout(MM_EnvironmentStandard *env)
{
	omrobjectptr_t *slotPtr;
	omrobjectptr_t objectPtr;
	MM_SublistPuddle *puddle;
	bool const compressed = _extensions->compressObjectReferences();

	if (_extensions->concurrentScavenger) {
		GC_SublistIterator remSetIterator(&(_extensions->rememberedSet));
		while((puddle = remSetIterator.nextList()) != NULL) {
			GC_SublistSlotIterator remSetSlotIterator(puddle);
			while((slotPtr = (omrobjectptr_t *)remSetSlotIterator.nextSlot()) != NULL) {
				objectPtr = *slotPtr;

				if (NULL == objectPtr) {
					remSetSlotIterator.removeSlot();
				} else {
					if((uintptr_t)objectPtr & DEFERRED_RS_REMOVE_FLAG) {
						/* Is slot flagged for deferred removal ? */
						/* Yes..so first remove tag bit from object address */
						objectPtr = (omrobjectptr_t)((uintptr_t)objectPtr & ~(uintptr_t)DEFERRED_RS_REMOVE_FLAG);
						Assert_MM_false(MM_ForwardedHeader(objectPtr, compressed).isForwardedPointer());

						/* The object did not have Nursery references at initial RS scan, but one could have been added during CS cycle by a mutator. */
						if (!shouldRememberObject(env, objectPtr)) {
							/* A simple mask out can be used - we are guaranteed to be the only manipulator of the object */
							_extensions->objectModel.clearRemembered(objectPtr);
							remSetSlotIterator.removeSlot();

							/* No need to inform anybody about creation of old-to-old reference (see regular pruning pass).
							 * For CS, this is already handled during scanning of old objects
							 */
						} else {
							/* We are not removing it after all, since the object has Nursery references => reset the deferred flag. */
							*slotPtr = objectPtr;
						}
					} else {
						/* Fixup newly remembered object */
						fixupObjectScan(env, objectPtr);
					}
				}
			}
		}
	} else {
		/* Walk the remembered set removing any tagged entries (back out of a tenured copy that is remembered)
		 * and scanning remembered objects for reverse fwd info
		 */

#if defined(OMR_SCAVENGER_TRACE_BACKOUT)
		OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());
		omrtty_printf("{SCAV: Back out RS list}\n");
#endif /* OMR_SCAVENGER_TRACE_BACKOUT */

		GC_SublistIterator remSetIterator(&(_extensions->rememberedSet));
		while((puddle = remSetIterator.nextList()) != NULL) {
			GC_SublistSlotIterator remSetSlotIterator(puddle);
			while((slotPtr = (omrobjectptr_t *)remSetSlotIterator.nextSlot()) != NULL) {
				/* clear any remove pending flags */
				*slotPtr = (omrobjectptr_t)((uintptr_t)*slotPtr & ~(uintptr_t)DEFERRED_RS_REMOVE_FLAG);
				objectPtr = *slotPtr;

				if(objectPtr) {
					if (MM_ForwardedHeader(objectPtr, compressed).isReverseForwardedPointer()) {
#if defined(OMR_SCAVENGER_TRACE_BACKOUT)
						omrtty_printf("{SCAV: Back out remove RS object %p[%p]}\n", objectPtr, *objectPtr);
#endif /* OMR_SCAVENGER_TRACE_BACKOUT */
						remSetSlotIterator.removeSlot();
					} else {
#if defined(OMR_SCAVENGER_TRACE_BACKOUT)
						omrtty_printf("{SCAV: Back out fixup RS object %p[%p]}\n", objectPtr, *objectPtr);
#endif /* OMR_SCAVENGER_TRACE_BACKOUT */
						backOutObjectScan(env, objectPtr);
					}
				} else {
					remSetSlotIterator.removeSlot();
				}
			}
		}
	}
}


void
MM_Scavenger::completeBackOut(MM_EnvironmentStandard *env)
{
	/* Work to be done (for non Concurrent Scavenger):
	 * 1) Flush copy scan caches
	 * 2) Walk the evacuate space, fixing up objects and installing reverse forward pointers in survivor space
	 * 3) Restore the remembered set
	 * 4) Client language completion of back out
	 */
	bool const compressed = _extensions->compressObjectReferences();

#if defined(OMR_SCAVENGER_TRACE_BACKOUT)
	OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());
#endif /* OMR_SCAVENGER_TRACE_BACKOUT */

	/* Ensure we've pushed all references from buffers out to the lists and flushed RS fragments*/
	flushBuffersForGetNextScanCache(env);

	/* Must synchronize to be sure all private caches have been flushed */
	if (env->_currentTask->synchronizeGCThreadsAndReleaseMain(env, UNIQUE_ID)) {
		setBackOutFlag(env, backOutStarted);

#if defined(OMR_SCAVENGER_TRACE_BACKOUT)
		omrtty_printf("{SCAV: Complete back out(%p)}\n", env->getLanguageVMThread());
#endif /* OMR_SCAVENGER_TRACE_BACKOUT */

		if (!IS_CONCURRENT_ENABLED) {
			/* 1) Flush copy scan caches */
			MM_CopyScanCacheStandard *cache = NULL;

			while (NULL != (cache = _scavengeCacheScanList.popCache(env))) {
				flushCache(env, cache);
			}
		}
		Assert_MM_true(0 == _cachedEntryCount);

		/* 2
		 * a) Mark the overflow scan as invalid (backing out of objects moved into old space)
		 * b) If the remembered set is in an overflow state,
		 *    i) Unremember any objects that moved from new space to old
		 *    ii) Walk old space and build up the overflow list
		 */
		_extensions->scavengerRsoScanUnsafe = true;

		if(isRememberedSetInOverflowState()) {
			GC_MemorySubSpaceRegionIterator evacuateRegionIterator(_activeSubSpace);
			MM_HeapRegionDescriptor* rootRegion;

#if defined(OMR_SCAVENGER_TRACE_BACKOUT)
			omrtty_printf("{SCAV: Handle RS overflow}\n");
#endif /* OMR_SCAVENGER_TRACE_BACKOUT */

			if (IS_CONCURRENT_ENABLED) {
				/* All heap fixup will occur during or after global GC */
				clearRememberedSetLists(env);
			} else {
				/* i) Unremember any objects that moved from new space to old */
				while(NULL != (rootRegion = evacuateRegionIterator.nextRegion())) {
					/* skip survivor regions */
					if (isObjectInEvacuateMemory((omrobjectptr_t)rootRegion->getLowAddress())) {
						/* tell the object iterator to work on the given region */
						GC_ObjectHeapIteratorAddressOrderedList evacuateHeapIterator(_extensions, rootRegion, false);
						evacuateHeapIterator.includeForwardedObjects();
						omrobjectptr_t objectPtr = NULL;
						omrobjectptr_t fwdObjectPtr = NULL;
						while((objectPtr = evacuateHeapIterator.nextObjectNoAdvance()) != NULL) {
							MM_ForwardedHeader header(objectPtr, compressed);
							fwdObjectPtr = header.getForwardedObject();
							if (NULL != fwdObjectPtr) {
								if(_extensions->objectModel.isRemembered(fwdObjectPtr)) {
									_extensions->objectModel.clearRemembered(fwdObjectPtr);
								}
#if defined(OMR_GC_DEFERRED_HASHCODE_INSERTION)
								evacuateHeapIterator.advance(_extensions->objectModel.getConsumedSizeInBytesWithHeaderBeforeMove(fwdObjectPtr));
#else
								evacuateHeapIterator.advance(_extensions->objectModel.getConsumedSizeInBytesWithHeader(fwdObjectPtr));
#endif /* defined(OMR_GC_DEFERRED_HASHCODE_INSERTION) */
							}
						}
					}
				}

				/* ii) Walk old space and build up the overflow list */
				/* the list is built because after reverse fwd ptrs are installed, the heap becomes unwalkable */
				clearRememberedSetLists(env);

				MM_RSOverflow rememberedSetOverflow(env);
				addAllRememberedObjectsToOverflow(env, &rememberedSetOverflow);

				/*
				 * 2.c)Walk the evacuate space, fixing up objects and installing reverse forward pointers in survivor space
				 */
				backoutFixupAndReverseForwardPointersInSurvivor(env);

				/* 3) Walk the remembered set, updating list pointers as well as remembered object ptrs */
#if defined(OMR_SCAVENGER_TRACE_BACKOUT)
				omrtty_printf("{SCAV: Back out RS overflow}\n");
#endif /* OMR_SCAVENGER_TRACE_BACKOUT */

				/* Walk the remembered set overflow list built earlier */
				omrobjectptr_t objectOverflow;
				while (NULL != (objectOverflow = rememberedSetOverflow.nextObject())) {
					backOutObjectScan(env, objectOverflow);
				}

				/* Walk all classes that are flagged as remembered */
				_delegate.backOutIndirectObjects(env);
			}
		} else {
			/* RS not in overflow */
			if (!IS_CONCURRENT_ENABLED) {
				/* Walk the evacuate space, fixing up objects and installing reverse forward pointers in survivor space */
				backoutFixupAndReverseForwardPointersInSurvivor(env);
			}

			processRememberedSetInBackout(env);

		} /* end of 'is RS in overflow' */

		MM_ScavengerBackOutScanner backOutScanner(env, true, this);
		backOutScanner.scanAllSlots(env);

#if defined(OMR_SCAVENGER_TRACE_BACKOUT)
		omrtty_printf("{SCAV: Done back out}\n");
#endif /* OMR_SCAVENGER_TRACE_BACKOUT */
		env->_currentTask->releaseSynchronizedGCThreads(env);
	}
}


/**
 * Setup, execute and complete a scavenge.
 */
void
MM_Scavenger::mainThreadGarbageCollect(MM_EnvironmentBase *envBase, MM_AllocateDescription *allocDescription, bool initMarkMap, bool rebuildMarkBits)
{
	OMRPORT_ACCESS_FROM_OMRPORT(envBase->getPortLibrary());
	MM_EnvironmentStandard *env = MM_EnvironmentStandard::getEnvironment(envBase);
	Trc_MM_Scavenger_mainThreadGarbageCollect_Entry(env->getLanguageVMThread());

	/* We might be running in a context of either main or main thread, but either way we must have exclusive access */
	Assert_MM_mustHaveExclusiveVMAccess(env->getOmrVMThread());

	if (_extensions->trackMutatorThreadCategory) {
		/* This thread is doing GC work, account for the time spent into the GC bucket */
		omrthread_set_category(env->getOmrVMThread()->_os_thread, J9THREAD_CATEGORY_SYSTEM_GC_THREAD, J9THREAD_TYPE_SET_GC);
	}

	Assert_MM_false(_currentPhaseConcurrent);

	bool firstIncrement = !isConcurrentCycleInProgress();

	if (firstIncrement)	{
		if (_extensions->processLargeAllocateStats) {
			processLargeAllocateStatsBeforeGC(env);
		}

		reportGCCycleStart(env);
		_extensions->scavengerStats._startTime = omrtime_hires_clock();
		mainSetupForGC(env);
	}
	clearIncrementGCStats(env, firstIncrement);
	reportGCStart(env);
	reportGCIncrementStart(env);
	reportScavengeStart(env);
	_extensions->incrementScavengerStats._startTime = omrtime_hires_clock();

	if (_extensions->concurrentScavenger) {
		scavengeIncremental(env);
	} else {
		scavenge(env);
	}

	bool lastIncrement = !isConcurrentCycleInProgress();
	_extensions->incrementScavengerStats._endTime = omrtime_hires_clock();

	/* merge stats from this increment/phase to aggregate cycle stats */
	mergeIncrementGCStats(env, lastIncrement);
	reportScavengeEnd(env, lastIncrement);

	if (lastIncrement) {
		/* defer to collector language interface */
		_delegate.mainThreadGarbageCollect_scavengeComplete(env);

		/* Reset the resizable flag of the semi space.
		 * NOTE: Must be done before we attempt to resize the new space.
		 */
		_activeSubSpace->setResizable(_cachedSemiSpaceResizableFlag);

		_extensions->scavengerStats._endTime = omrtime_hires_clock();

		if(scavengeCompletedSuccessfully(env)) {

			calculateRecommendedWorkingThreads(env);

			/* Merge sublists in the remembered set (if necessary) */
			_extensions->rememberedSet.compact(env);

			/* If -Xgc:fvtest=forcePoisonEvacuate has been specified, poison(fill poison pattern) evacuate space */
			if(_extensions->fvtest_forcePoisonEvacuate) {
				_activeSubSpace->poisonEvacuateSpace();
			}

			/* Build free list in evacuate profile. Perform resize. */
			_activeSubSpace->mainTeardownForSuccessfulGC(env);

			/* Defer to collector language interface */
			_delegate.mainThreadGarbageCollect_scavengeSuccess(env);

			if(_extensions->scvTenureStrategyAdaptive) {
				/* Adjust the tenure age based on the percentage of new space used.  Also, avoid / by 0 */
				uintptr_t newSpaceTotalSize = _activeSubSpace->getMemorySubSpaceAllocate()->getActiveMemorySize();
				uintptr_t newSpaceConsumedSize = _extensions->scavengerStats._flipBytes;
				uintptr_t newSpaceSizeScale = newSpaceTotalSize / 100;

				if((newSpaceConsumedSize < (_extensions->scvTenureRatioLow * newSpaceSizeScale)) && (_extensions->scvTenureAdaptiveTenureAge < OBJECT_HEADER_AGE_MAX)) {
					_extensions->scvTenureAdaptiveTenureAge++;
				} else {
					if((newSpaceConsumedSize > (_extensions->scvTenureRatioHigh * newSpaceSizeScale)) && (_extensions->scvTenureAdaptiveTenureAge > OBJECT_HEADER_AGE_MIN)) {
						_extensions->scvTenureAdaptiveTenureAge--;
					}
				}
			}
		} else {
			/* Build free list in survivor profile - the scavenge was unsuccessful, so rebuild the free list */
			_activeSubSpace->mainTeardownForAbortedGC(env);
		}
		/* Although evacuate is functionally irrelevant at this point since we are finishing the cycle,
		 * it is still useful for debugging (CS must not see live objects in Evacuate).
		 * Thus re-caching evacuate ranges to point to reserved/empty space of Survivor */
		_evacuateMemorySubSpace = _activeSubSpace->getMemorySubSpaceSurvivor();
		_activeSubSpace->cacheRanges(_evacuateMemorySubSpace, &_evacuateSpaceBase, &_evacuateSpaceTop);
		/* Restart the allocation caches associated to all threads */
		{
			GC_OMRVMThreadListIterator threadListIterator(_omrVM);
			OMR_VMThread *walkThread;
			while((walkThread = threadListIterator.nextOMRVMThread()) != NULL) {
				MM_EnvironmentBase *walkEnv = MM_EnvironmentBase::getEnvironment(walkThread);
				walkEnv->_objectAllocationInterface->restartCache(env);
			}
		}

		_extensions->heap->resetHeapStatistics(false);

		/* If there was a failed tenure of a size greater than the threshold, set the flag. */
		/* The next attempt to scavenge will result in a global collect */
		if (_extensions->scavengerStats._failedTenureCount > 0) {
			if (_extensions->scavengerStats._failedTenureBytes >= _extensions->scavengerFailedTenureThreshold) {
				Trc_MM_Scavenger_mainThreadGarbageCollect_setFailedTenureFlag(env->getLanguageVMThread(), _extensions->scavengerStats._failedTenureLargest);
				setFailedTenureThresholdFlag();
				setFailedTenureLargestObject(_extensions->scavengerStats._failedTenureLargest);
			}
		}
		if (_extensions->processLargeAllocateStats) {
			processLargeAllocateStatsAfterGC(env);
		}

		reportGCCycleFinalIncrementEnding(env);

	} // if lastIncrement


	reportGCIncrementEnd(env);
	reportGCEnd(env);
	if (lastIncrement) {
		reportGCCycleEnd(env);
		if (_extensions->processLargeAllocateStats) {
			/* reset tenure processLargeAllocateStats after TGC */
			resetTenureLargeAllocateStats(env);
		}
	}
	_extensions->allocationStats.clear();

	if (_extensions->trackMutatorThreadCategory) {
		/* Done doing GC, reset the category back to the old one */
		omrthread_set_category(env->getOmrVMThread()->_os_thread, 0, J9THREAD_TYPE_SET_GC);
	}

	Trc_MM_Scavenger_mainThreadGarbageCollect_Exit(env->getLanguageVMThread());
}


/**
 * Perform any pre-collection work as requested by the garbage collection invoker.
 */
void
MM_Scavenger::internalPreCollect(MM_EnvironmentBase *env, MM_MemorySubSpace *subSpace, MM_AllocateDescription *allocDescription, uint32_t gcCode)
{
#if defined(OMR_ENV_DATA64) && defined(OMR_GC_FULL_POINTERS)
	if (!env->compressObjectReferences()) {
		if (1 == _extensions->fvtest_enableReadBarrierVerification) {
			scavenger_healSlots(env);
		}
	}
#endif /* defined(OMR_ENV_DATA64) && defined(OMR_GC_FULL_POINTERS) */

	env->_cycleState = &_cycleState;

	/* Cycle state is initialized only once at the beginning of a cycle. We do not want, in mid-end cycle phases, to reset some members
	 * that are initialized at the beginning (such as verboseContextID).
	 */
	if (!isConcurrentCycleInProgress()) {
		_cycleState = MM_CycleState();
		_cycleState._gcCode = MM_GCCode(gcCode);
		_cycleState._type = _cycleType;
		_cycleState._collectionStatistics = &_collectionStatistics;

		/* If we are in an excessiveGC level beyond normal then an aggressive GC is
		 * conducted to free up as much space as possible
		 */
		if (!_cycleState._gcCode.isExplicitGC()) {
			if(excessive_gc_normal != _extensions->excessiveGCLevel) {
				/* convert the current mode to excessive GC mode */
				_cycleState._gcCode = MM_GCCode(J9MMCONSTANT_IMPLICIT_GC_EXCESSIVE);
			}
		}
	}

	/* Flush any VM level changes to prepare for a safe slot walk */
	GC_OMRVMInterface::flushCachesForGC(env);
}


/**
 * Internal API for invoking a garbage collect.
 * @return true if the collection completed successfully, false otherwise.
 */
bool
MM_Scavenger::internalGarbageCollect(MM_EnvironmentBase *envBase, MM_MemorySubSpace *subSpace, MM_AllocateDescription *allocDescription)
{
	MM_EnvironmentStandard *env = (MM_EnvironmentStandard *)envBase;
	MM_ScavengerStats *scavengerGCStats= &_extensions->scavengerStats;
	MM_MemorySubSpaceSemiSpace *subSpaceSemiSpace = (MM_MemorySubSpaceSemiSpace *)subSpace;
	MM_MemorySubSpace *tenureMemorySubSpace = subSpaceSemiSpace->getTenureMemorySubSpace();

	if (subSpaceSemiSpace->getMemorySubSpaceAllocate()->shouldAllocateAtSafePointOnly()) {
		/* There is no point in doing Scavenge, since we are about to complete Global, which will likely free up
		 * space in Nursery to satisfy AF that triggered this Scavenge.
		 * AllocateAtSafePointOnly flag is set exactly and only when Concurrent Mark execution mode is set to CONCURRENT_EXHAUSTED.
		 * Ideally, we should assert that the mode is CONCURRENT_EXHAUSTED, but Global GCs are not visible from here.
		 */
		Trc_MM_Scavenger_percolate_concurrentMarkExhausted(env->getLanguageVMThread());

		bool result = percolateGarbageCollect(env, subSpace, NULL, CONCURRENT_MARK_EXHAUSTED, J9MMCONSTANT_IMPLICIT_GC_PERCOLATE);

		Assert_MM_true(result);
		return true;
	}

	if (_extensions->concurrentScavenger && isBackOutFlagRaised()) {
		bool result = percolateGarbageCollect(env, subSpace, NULL, ABORTED_SCAVENGE, J9MMCONSTANT_IMPLICIT_GC_PERCOLATE_ABORTED_SCAVENGE);

		Assert_MM_true(result);

		return true;
	}

	/* First, if the previous scavenge had a failed tenure of a size greater than the threshold,
	 * ask parent MSS to try a collect.
	 */
	if (failedTenureThresholdReached()) {
		Trc_MM_Scavenger_percolate_failedTenureThresholdReached(env->getLanguageVMThread(), getFailedTenureLargestObject(), _extensions->heap->getPercolateStats()->getScavengesSincePercolate());

		/* Create an allocate description to describe the size of the
		 * largest chunk we need in the tenure space.
		 */
		MM_AllocateDescription percolateAllocDescription(getFailedTenureLargestObject(), OMR_GC_ALLOCATE_OBJECT_TENURED, false, true);

		/* We do an aggressive percolate if the last scavenge also percolated */
		uint32_t aggressivePercolate = _extensions->heap->getPercolateStats()->getScavengesSincePercolate() <= 1 ? J9MMCONSTANT_IMPLICIT_GC_PERCOLATE_AGGRESSIVE : J9MMCONSTANT_IMPLICIT_GC_PERCOLATE;

		/* Percolate the collect to parent MSS */
		bool result = percolateGarbageCollect(env, subSpace, &percolateAllocDescription, FAILED_TENURE, aggressivePercolate);

		/* Global GC must be executed */
		Assert_MM_true(result);

		/* Should have been reset by globalCollectionComplete() broadcast event */
		Assert_MM_true(!failedTenureThresholdReached());
		return true;
	}

	/*
	 * Second, if the previous scavenge failed to expand tenure, ask parent MSS to try a collect.
	 */
	if (expandFailed()) {
		Trc_MM_Scavenger_percolate_expandFailed(env->getLanguageVMThread());

		/* We do an aggressive percolate if the last scavenge also percolated */
		uint32_t aggressivePercolate = _extensions->heap->getPercolateStats()->getScavengesSincePercolate() <= 1 ? J9MMCONSTANT_IMPLICIT_GC_PERCOLATE_AGGRESSIVE : J9MMCONSTANT_IMPLICIT_GC_PERCOLATE;

		/* Aggressive percolate the collect to parent MSS */
		bool result = percolateGarbageCollect(env, subSpace, NULL, EXPAND_FAILED, aggressivePercolate);

		/* Global GC must be executed */
		Assert_MM_true(result);

		/* Should have been reset by globalCollectionComplete() broadcast event */
		Assert_MM_true(!expandFailed());
		return true;
	}

	/* If the tenure MSS is not expandable and/or  there is insufficent space left to tenure
	 * the average number of bytes tenured by a scavenge then percolate the collect to avoid
	 * an aborted scavenge and its associated time consuming backout
	 */
	if ((tenureMemorySubSpace->maxExpansionInSpace(env) + tenureMemorySubSpace->getApproximateActiveFreeMemorySize()) < scavengerGCStats->_avgTenureBytes ) {
		Trc_MM_Scavenger_percolate_insufficientTenureSpace(env->getLanguageVMThread(), tenureMemorySubSpace->maxExpansionInSpace(env), tenureMemorySubSpace->getApproximateActiveFreeMemorySize(), scavengerGCStats->_avgTenureBytes);

		/* Percolate the collect to parent MSS */
		bool result = percolateGarbageCollect(env, subSpace, NULL, INSUFFICIENT_TENURE_SPACE, J9MMCONSTANT_IMPLICIT_GC_PERCOLATE);

		/* Global GC must be executed */
		Assert_MM_true(result);
		return true;
	}

	/* If it has been too long since a global GC, execute one instead of a scavenge. */
	//TODO Probably should rename this -Xgc option as it may not always result in a ggc
	//in futre, e.g if we implement multiple generations.
	if (_extensions->maxScavengeBeforeGlobal) {
		if (_countSinceForcingGlobalGC++ >= _extensions->maxScavengeBeforeGlobal) {
			Trc_MM_Scavenger_percolate_maxScavengeBeforeGlobal(env->getLanguageVMThread(), _extensions->maxScavengeBeforeGlobal);

			/* Percolate the collect to parent MSS */
			bool result = percolateGarbageCollect(env, subSpace, NULL, MAX_SCAVENGES, J9MMCONSTANT_IMPLICIT_GC_PERCOLATE);

			/* Global GC must be executed */
			Assert_MM_true(result);

			/* Should have been reset by globalCollectionComplete() broadcast event */
			Assert_MM_true(_countSinceForcingGlobalGC == 0);
			return true;
		}
	}

#if defined(OMR_GC_MODRON_CONCURRENT_MARK)
	if (!_extensions->concurrentMark) {
			uintptr_t previousUsedOldHeap = _extensions->oldHeapSizeOnLastGlobalGC - _extensions->freeOldHeapSizeOnLastGlobalGC;
			float maxTenureFreeRatio = _extensions->heapFreeMaximumRatioMultiplier / 100.0f;
			float midTenureFreeRatio = (_extensions->heapFreeMinimumRatioMultiplier + _extensions->heapFreeMaximumRatioMultiplier) / 200.0f;
			uintptr_t soaFreeMemorySize = _extensions->heap->getApproximateActiveFreeMemorySize(MEMORY_TYPE_OLD) - _extensions->heap->getApproximateActiveFreeLOAMemorySize(MEMORY_TYPE_OLD);

			/* We suspect that next scavenge will cause Tenure expansion, while Tenure free ratio is (was) already high enough */
			if ((scavengerGCStats->_avgTenureBytes > soaFreeMemorySize) && (_extensions->freeOldHeapSizeOnLastGlobalGC > (_extensions->oldHeapSizeOnLastGlobalGC * midTenureFreeRatio))) {
				Trc_MM_Scavenger_percolate_preventTenureExpand(env->getLanguageVMThread());

				bool result = percolateGarbageCollect(env, subSpace, NULL, PREVENT_TENURE_EXPAND, J9MMCONSTANT_IMPLICIT_GC_PERCOLATE);
				Assert_MM_true(result);
				return true;
			}
			/* There was Tenure heap growth since last Global GC, and we suspect (assuming live set size did not grow) we might be going beyond max free bound. */
			if ((_extensions->heap->getActiveMemorySize(MEMORY_TYPE_OLD) > _extensions->oldHeapSizeOnLastGlobalGC) && (_extensions->heap->getActiveMemorySize(MEMORY_TYPE_OLD) * maxTenureFreeRatio < (_extensions->heap->getActiveMemorySize(MEMORY_TYPE_OLD) - previousUsedOldHeap))) {
				Trc_MM_Scavenger_percolate_tenureMaxFree(env->getLanguageVMThread());

				bool result = percolateGarbageCollect(env, subSpace, NULL, MET_PROJECTED_TENURE_MAX_FREE, J9MMCONSTANT_IMPLICIT_GC_PERCOLATE);
				Assert_MM_true(result);
				return true;
			}
	}
#endif /* OMR_GC_MODRON_CONCURRENT_MARK */

	/**
	 * Language percolation trigger
	 * Allow the CollectorLanguageInterface to advise if percolation should occur.
	 */
	PercolateReason percolateReason = NONE_SET;
	uint32_t gcCode = J9MMCONSTANT_IMPLICIT_GC_DEFAULT;

	bool shouldPercolate = _delegate.internalGarbageCollect_shouldPercolateGarbageCollect(env, & percolateReason, & gcCode);
	if (shouldPercolate) {
		Trc_MM_Scavenger_percolate_delegate(env->getLanguageVMThread());

		bool didPercolate = percolateGarbageCollect(env, subSpace, NULL, percolateReason, gcCode);
		/* Percolation must occur if required by the cli. */
		if (didPercolate) {
			return true;
		}
	}

	/* Check if there is an RSO and the heap is not safely walkable */
	if(isRememberedSetInOverflowState() && _extensions->scavengerRsoScanUnsafe) {
		/* NOTE: No need to set that the collect was unsuccessful - we will actually execute
		 * the scavenger after percolation.
		 */

		Trc_MM_Scavenger_percolate_rememberedSetOverflow(env->getLanguageVMThread());

		/* Percolate the collect to parent MSS */
		percolateGarbageCollect(env, subSpace, NULL, RS_OVERFLOW, J9MMCONSTANT_IMPLICIT_GC_PERCOLATE);
	}

	_extensions->heap->getPercolateStats()->incrementScavengesSincePercolate();

	_extensions->scavengerStats._gcCount += 1;
	env->_cycleState->_activeSubSpace = subSpace;
	_collectorExpandedSize = 0;

	if (_extensions->concurrentScavenger) {
		/* this may trigger either start or end of Concurrent Scavenge cycle */
		triggerConcurrentScavengerTransition(env, allocDescription);
	} else {
		mainThreadGarbageCollect(env, allocDescription);
	}

	/* If we know now that the next scavenge will cause a peroclate broadcast
	 * the fact so other parties can react, e.g concurrrent can adjust KO threshold
	 */

	if (failedTenureThresholdReached()
		|| expandFailed()
		|| (_extensions->maxScavengeBeforeGlobal && _countSinceForcingGlobalGC == _extensions->maxScavengeBeforeGlobal)
		|| ((tenureMemorySubSpace->maxExpansionInSpace(env) + tenureMemorySubSpace->getApproximateActiveFreeMemorySize()) < scavengerGCStats->_avgTenureBytes)) {
		_extensions->scavengerStats._nextScavengeWillPercolate = true;
	}

	return true;
}

bool
MM_Scavenger::isConcurrentWorkAvailable(MM_EnvironmentBase *env)
{
	return (concurrent_phase_scan == _concurrentPhase);
}

bool
MM_Scavenger::scavengeInit(MM_EnvironmentBase *env)
{
	GC_OMRVMThreadListIterator threadIterator(_extensions->getOmrVM());
	OMR_VMThread *walkThread = NULL;

	while((walkThread = threadIterator.nextOMRVMThread()) != NULL) {
		MM_EnvironmentStandard *threadEnvironment = MM_EnvironmentStandard::getEnvironment(walkThread);
		if (MUTATOR_THREAD == threadEnvironment->getThreadType()) {
			// we have to do a subset of setup operations that GC workers do
			// possibly some of those also to be done of thread (env) initialization
			mutatorSetupForGC(threadEnvironment);
		}
	}
	return false;
}

bool
MM_Scavenger::scavengeRoots(MM_EnvironmentBase *env)
{
	Assert_MM_true(concurrent_phase_roots == _concurrentPhase);

	MM_ConcurrentScavengeTask scavengeTask(env, _dispatcher, this, MM_ConcurrentScavengeTask::SCAVENGE_ROOTS, env->_cycleState);
	_dispatcher->run(env, &scavengeTask);

	return false;
}

bool
MM_Scavenger::scavengeScan(MM_EnvironmentBase *envBase)
{
	Assert_MM_true(concurrent_phase_scan == _concurrentPhase);
	/* In rare cases, we may end up at later phases of the cycle in STW without even having a chance
	 * to run concurrent phase which normally checks and clears the yield flag.
	 * TODO: consider reporting the source of yeild request in VGC, similarly as we do for concurrent phase */
	_shouldYield = false;

	MM_EnvironmentStandard *env = MM_EnvironmentStandard::getEnvironment(envBase);

	restoreMainThreadTenureTLHRemainders(env);

	MM_ConcurrentScavengeTask scavengeTask(env, _dispatcher, this, MM_ConcurrentScavengeTask::SCAVENGE_SCAN, env->_cycleState);
	_dispatcher->run(env, &scavengeTask);

	return false;
}

bool
MM_Scavenger::scavengeComplete(MM_EnvironmentBase *envBase)
{
	MM_EnvironmentStandard *env = MM_EnvironmentStandard::getEnvironment(envBase);

	Assert_MM_true(concurrent_phase_complete == _concurrentPhase);
	/* We may end up at later phases of the cycle in STW without even having a chance
	 * to run concurrent phase which normally checks and clears the yield flag. */
	_shouldYield = false;

	restoreMainThreadTenureTLHRemainders(env);

	MM_ConcurrentScavengeTask scavengeTask(env, _dispatcher, this, MM_ConcurrentScavengeTask::SCAVENGE_COMPLETE, env->_cycleState);
	_dispatcher->run(env, &scavengeTask);

	Assert_MM_true(_scavengeCacheFreeList.areAllCachesReturned());

	return false;
}

void
MM_Scavenger::mutatorSetupForGC(MM_EnvironmentBase *envBase)
{
	MM_EnvironmentStandard *env = MM_EnvironmentStandard::getEnvironment(envBase);

	if (isConcurrentCycleInProgress()) {
		/* caches should all be reset */
		Assert_MM_true(NULL == env->_survivorCopyScanCache);
		Assert_MM_true(NULL == env->_tenureCopyScanCache);
		Assert_MM_true(NULL == env->_deferredScanCache);
		Assert_MM_true(NULL == env->_deferredCopyCache);
		Assert_MM_false(env->_loaAllocation);
		Assert_MM_true(NULL == env->_survivorTLHRemainderBase);
		Assert_MM_true(NULL == env->_survivorTLHRemainderTop);
	}
}

MMINLINE void
MM_Scavenger::flushInactiveSurvivorCopyScanCache(MM_EnvironmentStandard *currentEnv, MM_EnvironmentStandard *targetEnv, bool flushCaches, bool final)
{
	MM_CopyScanCacheStandard *cache = (MM_CopyScanCacheStandard *)targetEnv->_inactiveSurvivorCopyScanCache;
	if (NULL != cache) {
		/* Either we are explicitly instructed to flush, or we are observing suboptimal parallelism in background threads */
		if (flushCaches || (_waitingCount > 0)) {
			/* Racing with mutator trying to activate cache */
			if ((uintptr_t)cache == MM_AtomicOperations::lockCompareExchange((volatile uintptr_t *)&targetEnv->_inactiveSurvivorCopyScanCache, (uintptr_t)cache, (uintptr_t)NULL)) {
				/* It's already cleared on the way it became inactive */
				Assert_MM_true(0 != (cache->flags & (OMR_SCAVENGER_CACHE_TYPE_COPY | OMR_SCAVENGER_CACHE_TYPE_CLEARED)));
				cache->flags &= ~OMR_SCAVENGER_CACHE_TYPE_COPY;

#if defined(J9MODRON_TGC_PARALLEL_STATISTICS)
				currentEnv->_scavengerStats._releaseScanListCount += 1;
#endif /* J9MODRON_TGC_PARALLEL_STATISTICS */
				/* notify is only needed by the end of concurrent phase */
				addCacheEntryToScanListAndNotify(targetEnv, cache);
			}
		}
		/* else thread has had back-to-back VM access release without any barriers in between to activate this copy cache */
	}
}

MMINLINE void
MM_Scavenger::deactivateSurvivorCopyScanCache(MM_EnvironmentStandard *currentEnv, MM_EnvironmentStandard *targetEnv, bool flushCaches, bool final)
{
	/* Typically called from a mutator thread releasing VM access, to deactivate the cashe.
	 * Also can be called from Async Handler toward end of concurrent phase to flush it.
	 * In both cases: (currentEnv == targetEnv).
	 * But can also be called from another mutator thread (currentEnv != targetEnv), when doing a final flush mutator walk before STW operation
	 * or even when doing exclusive VM access mutator walk if thread was in Native.
	 */
	MM_CopyScanCacheStandard *cache = targetEnv->_survivorCopyScanCache;
	if (NULL != cache) {
		if ((currentEnv == targetEnv) || final || targetEnv->inNative()) {
			/* Race between a calling GC thread and target thread (potentially refreshing cache) is not expected:
			 * 1) if currentEnv == targetEnv, it's obvious
			 * 2) if final, then we are at the start of STW, while all target threads have already blocked
			 * 3) if inNative, target thread cannot be executing barrier (and GC code); if it races to exit native,
			 *    it will block since a) it's forced to reacquire VM access through slow path and b) caller holds thread public mutex what prevents reacquire
			 */
			targetEnv->_survivorCopyScanCache = NULL;
			bool remainderCreated = clearCache(targetEnv, cache);
			if (flushCaches || (_waitingCount > 0) || !remainderCreated) {
				/* Either we are explicitly instructed to flush, or we are observing suboptimal parallelism in background threads,
				 * or no free space in the cache -> don't deactivate, but just push it for scanning  */
				Assert_MM_true(0 != (cache->flags & OMR_SCAVENGER_CACHE_TYPE_COPY));
				cache->flags &= ~OMR_SCAVENGER_CACHE_TYPE_COPY;
#if defined(J9MODRON_TGC_PARALLEL_STATISTICS)
				targetEnv->_scavengerStats._releaseScanListCount += 1;
#endif /* J9MODRON_TGC_PARALLEL_STATISTICS */
				addCacheEntryToScanListAndNotify(targetEnv, cache);
			} else {
				/* If it's an intermediate release (mutator threads releasing VM access in a middle of concurrent cycle), and high parallelism,
				 * we'll keep the cache around, but tag it inactive, so it can be safely flushed at a later point (toward the end of concurrent phase)
				 * by another thread. Setting inactive cache flag, just for debugging purposes.
				 */
				Assert_MM_true(NULL == targetEnv->_inactiveSurvivorCopyScanCache);
				targetEnv->_inactiveSurvivorCopyScanCache = cache;
			}
		}
	}

}

MMINLINE void
MM_Scavenger::flushInactiveTenureCopyScanCache(MM_EnvironmentStandard *currentEnv, MM_EnvironmentStandard *targetEnv, bool flushCaches, bool final)
{
	MM_CopyScanCacheStandard *cache = (MM_CopyScanCacheStandard *)targetEnv->_inactiveTenureCopyScanCache;
	if (NULL != cache) {
		/* Either we are explicitly instructed to flush, or we are observing suboptimal parallelism in background threads */
		if (flushCaches || (_waitingCount > 0)) {
			/* Racing with mutator trying to activate cache */
			if ((uintptr_t)cache == MM_AtomicOperations::lockCompareExchange((volatile uintptr_t *)&targetEnv->_inactiveTenureCopyScanCache, (uintptr_t)cache, (uintptr_t)NULL)) {
				/* It's already cleared on the way it became inactive */
				Assert_MM_true(0 != (cache->flags & (OMR_SCAVENGER_CACHE_TYPE_COPY | OMR_SCAVENGER_CACHE_TYPE_CLEARED)));
				cache->flags &= ~OMR_SCAVENGER_CACHE_TYPE_COPY;

#if defined(J9MODRON_TGC_PARALLEL_STATISTICS)
				targetEnv->_scavengerStats._releaseScanListCount += 1;
#endif /* J9MODRON_TGC_PARALLEL_STATISTICS */
				addCacheEntryToScanListAndNotify(targetEnv, cache);
			}
			/* else we failed to push inactive cache since mutator thread reactivated it -> concurrent cycle continues */
		}
		/* else thread has had consecutive VM access release without any barriers in between to activate this copy cache */
	}
}

MMINLINE void
MM_Scavenger::deactivateTenureCopyScanCache(MM_EnvironmentStandard *currentEnv, MM_EnvironmentStandard *targetEnv, bool flushCaches, bool final)
{
	MM_CopyScanCacheStandard *cache = targetEnv->_tenureCopyScanCache;
	if (NULL != cache) {
		if ((currentEnv == targetEnv) || final || targetEnv->inNative()) {
			/* Race between a calling GC thread and target thread (potentially refreshing cache) is not expected:
			 * 1) if currentEnv == targetEnv, it's obvious
			 * 2) if final, then we are at the start of STW, while all target threads have already blocked
			 * 3) if inNative, target thread cannot be executing barrier (and GC code); if it races to exit native,
			 *    it will block since a) it's forced to reacquire VM access through slow path and b) caller holds thread public mutex what prevents reacquire
			 */
			targetEnv->_tenureCopyScanCache = NULL;
			bool remainderCreated = clearCache(targetEnv, cache);
			if (flushCaches || (_waitingCount > 0) || !remainderCreated) {
				/* Either we are explicitly instructed to flush, or we are observing suboptimal parallelism in background threads,
				 * or no free space in the cache -> don't deactivate, but just push it for scanning  */
				Assert_MM_true(0 != (cache->flags & OMR_SCAVENGER_CACHE_TYPE_COPY));
				cache->flags &= ~OMR_SCAVENGER_CACHE_TYPE_COPY;
#if defined(J9MODRON_TGC_PARALLEL_STATISTICS)
				targetEnv->_scavengerStats._releaseScanListCount += 1;
#endif /* J9MODRON_TGC_PARALLEL_STATISTICS */
				addCacheEntryToScanListAndNotify(targetEnv, cache);
			} else {
				/* If it's an intermediate release (mutator threads releasing VM access in a middle of concurrent phase), and high parallelism,
				 * we'll keep the cache around, but tag it inactive, so it can be safely flushed at a later point (toward the end of concurrent phase)
				 * by another thread. Setting inactive cache flag, just for debugging purposes.
				 */
				Assert_MM_true(NULL == targetEnv->_inactiveTenureCopyScanCache);
				targetEnv->_inactiveTenureCopyScanCache = cache;
			}
		}
	}
}


MMINLINE void
MM_Scavenger::flushInactiveDeferredCopyScanCache(MM_EnvironmentStandard *currentEnv, MM_EnvironmentStandard *targetEnv, bool flushCaches, bool final)
{
	MM_CopyScanCacheStandard *cache = (MM_CopyScanCacheStandard *)targetEnv->_inactiveDeferredCopyCache;
	if (NULL != cache) {
		/* Either we are explicitly instructed to flush, or we are observing suboptimal parallelism in background threads */
		if (flushCaches || (_waitingCount > 0)) {
			/* Racing with mutator trying to activate cache */
			if ((uintptr_t)cache == MM_AtomicOperations::lockCompareExchange((volatile uintptr_t *)&targetEnv->_inactiveDeferredCopyCache, (uintptr_t)cache, (uintptr_t)NULL)) {
				Assert_MM_true(0 != (cache->flags & OMR_SCAVENGER_CACHE_TYPE_COPY));
				cache->flags &= ~OMR_SCAVENGER_CACHE_TYPE_COPY;

#if defined(J9MODRON_TGC_PARALLEL_STATISTICS)
				targetEnv->_scavengerStats._releaseScanListCount += 1;
#endif /* J9MODRON_TGC_PARALLEL_STATISTICS */
				addCacheEntryToScanListAndNotify(targetEnv, cache);
			}
			/* else we failed to push inactive cache since mutator thread reactivated it -> concurrent cycle continues */
		}
		/* else thread has had consecutive VM access release without any barriers in between to activate this copy cache */
	}
}

MMINLINE void
MM_Scavenger::deactivateDeferredCopyScanCache(MM_EnvironmentStandard *currentEnv, MM_EnvironmentStandard *targetEnv, bool flushCaches, bool final)
{
	MM_CopyScanCacheStandard *cache = targetEnv->_deferredCopyCache;
	if (NULL != cache) {
		/* We cannot allow dangling deferred copy cache - if we just pushed survivor or tenure copy cache we should push deferred, too. */
		if ((currentEnv == targetEnv) || final || targetEnv->inNative()
				|| (NULL == targetEnv->_survivorCopyScanCache) || (NULL == targetEnv->_tenureCopyScanCache)) {
			/* Race between mutator thread actual owner and another mutator thread that helps with concurrent termination */
			if ((uintptr_t)cache == MM_AtomicOperations::lockCompareExchange((volatile uintptr_t *)&targetEnv->_deferredCopyCache, (uintptr_t)cache, (uintptr_t)NULL)) {
				targetEnv->_deferredCopyCache = NULL;
				Assert_MM_true(0 != (cache->flags & OMR_SCAVENGER_CACHE_TYPE_CLEARED));
				if (flushCaches || (_waitingCount > 0) || (NULL == targetEnv->_survivorCopyScanCache) || (NULL == targetEnv->_tenureCopyScanCache)) {
					/* Main thread in STW is MUTATOR type and can trigger this as well */
					Assert_MM_true(0 != (cache->flags & OMR_SCAVENGER_CACHE_TYPE_COPY));
					cache->flags &= ~OMR_SCAVENGER_CACHE_TYPE_COPY;
#if defined(J9MODRON_TGC_PARALLEL_STATISTICS)
					targetEnv->_scavengerStats._releaseScanListCount += 1;
#endif /* J9MODRON_TGC_PARALLEL_STATISTICS */
					addCacheEntryToScanListAndNotify(targetEnv, cache);
				} else {
					Assert_MM_true(NULL == targetEnv->_inactiveDeferredCopyCache);
					targetEnv->_inactiveDeferredCopyCache = cache;
				}
			}
		}
	}
}

/* TODO: remove once threadReleaseCaches gets currentEnv from callers */
static OMR_VMThread *
getCurrentOMRVMThread(OMR_VM *vm)
{
	OMR_VMThread *currentThread = NULL;
	omrthread_t self = omrthread_self();
	if (NULL != self) {
		if (vm->_vmThreadKey > 0) {
			currentThread = (OMR_VMThread *)omrthread_tls_get(self, vm->_vmThreadKey);
		}
	}
	return currentThread;
}

void
MM_Scavenger::threadReleaseCaches(MM_EnvironmentBase *currentEnvBase, MM_EnvironmentBase *targetEnvBase, bool flushCaches, bool final)
{
	MM_EnvironmentStandard *targetEnv = MM_EnvironmentStandard::getEnvironment(targetEnvBase);

	Assert_MM_true(flushCaches >= final);

	if (isConcurrentCycleInProgress()) {
		/* TODO: have callers pass currentEnv */
		MM_EnvironmentStandard *currentEnv = NULL;
		if (NULL == currentEnvBase) {
			currentEnv = MM_EnvironmentStandard::getEnvironment(getCurrentOMRVMThread(targetEnvBase->getOmrVM()));
		} else {
			currentEnv = MM_EnvironmentStandard::getEnvironment(currentEnvBase);
		}

		if (NULL != targetEnv->_deferredScanCache) {
			Assert_MM_true(MUTATOR_THREAD != targetEnv->getThreadType());
#if defined(J9MODRON_TGC_PARALLEL_STATISTICS)
			targetEnv->_scavengerStats._releaseScanListCount += 1;
#endif /* J9MODRON_TGC_PARALLEL_STATISTICS */
			_scavengeCacheScanList.pushCache(targetEnv, targetEnv->_deferredScanCache);
			targetEnv->_deferredScanCache = NULL;
		}

		flushInactiveSurvivorCopyScanCache(currentEnv, targetEnv, flushCaches, final);
		deactivateSurvivorCopyScanCache(currentEnv, targetEnv, flushCaches, final);
		flushInactiveTenureCopyScanCache(currentEnv, targetEnv, flushCaches, final);
		deactivateTenureCopyScanCache(currentEnv, targetEnv, flushCaches, final);
		flushInactiveDeferredCopyScanCache(currentEnv, targetEnv, flushCaches, final);
		deactivateDeferredCopyScanCache(currentEnv, targetEnv, flushCaches, final);

		if (final) {
			/* If it's an intermediate release (mutator threads releasing VM access in a middle of Concurrent Scavenger cycle),
			 * keep copy cache remainders around (do not abandon yet), to be reused if the threads re-acquires VM access during the same CS cycle.
			 * For final release, we abondon ever remainders.
			 */
			abandonSurvivorTLHRemainder(targetEnv);
			abandonTenureTLHRemainder(targetEnv, true);
		}
	}
}

bool
MM_Scavenger::scavengeIncremental(MM_EnvironmentBase *env)
{
	Assert_MM_mustHaveExclusiveVMAccess(env->getOmrVMThread());
	bool result = false;
	bool timeout = false;

	while (!timeout) {

		switch (_concurrentPhase) {
		case concurrent_phase_idle:
		{
			_concurrentPhase = concurrent_phase_init;
			continue;
		}
		case concurrent_phase_init:
		{
			/* initialize the mark map */
			scavengeInit(env);

			_concurrentPhase = concurrent_phase_roots;
		}
			break;

		case concurrent_phase_roots:
		{
			/* initialize all the roots */
			scavengeRoots(env);

			_activeSubSpace->flip(env, MM_MemorySubSpaceSemiSpace::set_allocate);

			/* prepare for the second pass (direct refs) */
			_extensions->rememberedSet.startProcessingSublist();

			_concurrentPhase = concurrent_phase_scan;

			if (isBackOutFlagRaised()) {
				/* if we aborted during root processing, continue with the cycle while still in STW mode */
				mergeIncrementGCStats(env, false);
				clearIncrementGCStats(env, false);
				continue;
			}

			timeout = true;
		}
			break;

		case concurrent_phase_scan:
		{
			/* This is just for corner cases that must be run in STW mode.
			 * Default main scan phase is done within mainThreadConcurrentCollect. */

			timeout = scavengeScan(env);

			_concurrentPhase = concurrent_phase_complete;

			mergeIncrementGCStats(env, false);
			clearIncrementGCStats(env, false);
			continue;
		}

		case concurrent_phase_complete:
		{
			scavengeComplete(env);

			result = true;
			_concurrentPhase = concurrent_phase_idle;
			timeout = true;
		}
			break;

		default:
			Assert_MM_unreachable();
		}
	}

	return result;
}

void
MM_Scavenger::workThreadProcessRoots(MM_EnvironmentStandard *env)
{
	workerSetupForGC(env);

	MM_ScavengerRootScanner rootScanner(env, this);

	/* Indirect refs, only. */
	rootScanner.scavengeRememberedSet(env);

	rootScanner.scanRoots(env);

	/* Push any thread local copy caches to scan queue and abandon unused memory to make it walkable.
	 * This is important to do only for GC threads that will not be used in concurrent phase, but at this point
	 * we don't know which threads Scheduler will not use, so we do it for every thread.
	 */
	threadReleaseCaches(env, env, true, true);

	mergeThreadGCStats(env);
}

void
MM_Scavenger::workThreadScan(MM_EnvironmentStandard *env)
{
	/* This is where the most of scan work should occur in CS. Typically as a concurrent task (background threads), but in some corner cases it could be scheduled as a STW task */
	clearThreadGCStats(env, false);

	/* Direct refs, only. */
	MM_ScavengerRootScanner rootScanner(env, this);
	rootScanner.scavengeRememberedSet(env);

	completeScan(env);

	/* We might have yielded without exausting scan work. Push any open caches to the scan queue, so that GC threads from final STW phase pick them up.
	 * Most of the time, STW phase will have a superset of GC threads, so they could just resume the work on their own caches,
	 * but this is not 100% guarantied (the control of what threads are inolved is in Dispatcher's domain).
	 */
	threadReleaseCaches(env, env, true, true);

	mergeThreadGCStats(env);
}

void
MM_Scavenger::workThreadComplete(MM_EnvironmentStandard *env)
{
	Assert_MM_true(_extensions->concurrentScavenger);

	/* record that this thread is participating in this cycle */
	env->_scavengerStats._gcCount = _extensions->scavengerStats._gcCount;

	clearThreadGCStats(env, false);

	MM_ScavengerRootScanner rootScanner(env, this);

	/* Complete scan loop regardless if we already aborted. If so, the scan operation will just fix up pointers that still point to forwarded objects.
	 * This is important particularly for Tenure space where recovery procedure will not walk the Tenure space for exhaustive fixup.
	 */
	completeScan(env);

	if (!isBackOutFlagRaised()) {
		/* If aborted, the clearable work will be done by mandatory percolate global GC */
		rootScanner.scanClearable(env);
	}
	rootScanner.flush(env);

	finalReturnCopyCachesToFreeList(env);
	abandonSurvivorTLHRemainder(env);
	abandonTenureTLHRemainder(env, true);

	/* If -Xgc:fvtest=forceScavengerBackout has been specified, set backout flag every 3rd scavenge */
	if(_extensions->fvtest_forceScavengerBackout) {
		if (env->_currentTask->synchronizeGCThreadsAndReleaseMain(env, UNIQUE_ID)) {
			if (2 <= _extensions->fvtest_backoutCounter) {
#if defined(OMR_SCAVENGER_TRACE_BACKOUT)
				OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());
				omrtty_printf("{SCAV: Forcing back out(%p)}\n", env->getLanguageVMThread());
#endif /* OMR_SCAVENGER_TRACE_BACKOUT */
				setBackOutFlag(env, backOutFlagRaised);
				_extensions->fvtest_backoutCounter = 0;
			} else {
				_extensions->fvtest_backoutCounter += 1;
			}
			env->_currentTask->releaseSynchronizedGCThreads(env);
		}
	}

	if(isBackOutFlagRaised()) {
		env->_scavengerStats._backout = 1;
		completeBackOut(env);
	} else {
		/* pruning */
		rootScanner.pruneRememberedSet(env);
	}

	/* No matter what happens, always sum up the gc stats */
	mergeThreadGCStats(env);
}

uintptr_t
MM_Scavenger::mainThreadConcurrentCollect(MM_EnvironmentBase *env)
{
	if (concurrent_phase_scan == _concurrentPhase) {
		clearIncrementGCStats(env, false);

		_currentPhaseConcurrent = true;
		/* We claim to work concurrently. GC better not have exclusive VM access. We can really only assert it for the current thread */
		Assert_MM_true(0 == env->getOmrVMThread()->exclusiveCount);

		MM_ConcurrentScavengeTask scavengeTask(env, _dispatcher, this, MM_ConcurrentScavengeTask::SCAVENGE_SCAN, env->_cycleState);
		/* Concurrent background task will run with different (typically lower) number of threads. */
		_dispatcher->run(env, &scavengeTask, _extensions->concurrentScavengerBackgroundThreads);

		_currentPhaseConcurrent = false;

		/* Now that we are done with concurrent scanning in this cycle (where we could possibly
		 * be interested in its value), record shouldYield Flag for reporting purposes and reset it. */
		if (_shouldYield) {
			if (NULL == _extensions->gcExclusiveAccessThreadId) {
				/* We terminated concurrent cycle due to a external request. We will not move to 'complete' phase,
				 * but stay in concurrent scan phase and try to resume work after the external party is done
				 * (when we are able to regain VM access)
				 */
				getConcurrentPhaseStats()->_terminationRequestType = MM_ConcurrentPhaseStatsBase::terminationRequest_External;
			} else {
				/* Ran out of free space in allocate/survivor, or system/global GC */
				getConcurrentPhaseStats()->_terminationRequestType = MM_ConcurrentPhaseStatsBase::terminationRequest_ByGC;
			}
			_shouldYield = false;
		} else {
			/* Exhausted scan work */
			_concurrentPhase = concurrent_phase_complete;

			/* make allocate space non-allocatable to trigger the next GC phase */
			_activeSubSpace->flip(env, MM_MemorySubSpaceSemiSpace::disable_allocation);
		}

		mergeIncrementGCStats(env, false);

		_delegate.cancelSignalToFlushCaches(env);

		/* return the number of bytes scanned since the caller needs to pass it into postConcurrentUpdateStatsAndReport for stats reporting */
		return scavengeTask.getBytesScanned();
	} else {
		/* someone else might have done this phase (and the rest of the cycle), forced in STW, before we even got a chance to run. */
		Assert_MM_true(concurrent_phase_idle == _concurrentPhase);
		return 0;
	}
}

void MM_Scavenger::preConcurrentInitializeStatsAndReport(MM_EnvironmentBase *env, MM_ConcurrentPhaseStatsBase *stats)
{
	OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());
	Assert_MM_true(NULL == env->_cycleState);
	env->_cycleState = &_cycleState;

	stats->_cycleID = _cycleState._verboseContextID;

	TRIGGER_J9HOOK_MM_PRIVATE_CONCURRENT_PHASE_START(
			_extensions->privateHookInterface,
			env->getOmrVMThread(),
			omrtime_hires_clock(),
			J9HOOK_MM_PRIVATE_CONCURRENT_PHASE_START,
			stats);

	_extensions->incrementScavengerStats._startTime = omrtime_hires_clock();
}

void MM_Scavenger::postConcurrentUpdateStatsAndReport(MM_EnvironmentBase *env, MM_ConcurrentPhaseStatsBase *stats, UDATA bytesConcurrentlyScanned)
{
	OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());

	_extensions->incrementScavengerStats._endTime = omrtime_hires_clock();

	TRIGGER_J9HOOK_MM_PRIVATE_CONCURRENT_PHASE_END(
		_extensions->privateHookInterface,
		env->getOmrVMThread(),
		omrtime_hires_clock(),
		J9HOOK_MM_PRIVATE_CONCURRENT_PHASE_END,
		stats);

	env->_cycleState = NULL;
}


void
MM_Scavenger::switchConcurrentForThread(MM_EnvironmentBase *env)
{
	/* If a thread local counter is behind the global one (or ahead in case of a rollover), we need to trigger a switch.
	 * It means we recently transitioned from a cycle start to cycle end or vice versa.
	 * If state is idle, we just completed a cycle. If state is scan (or rarely complete), we just started a cycle (and possibly even complete concurrent work)
	 */

   	Assert_MM_false((concurrent_phase_init == _concurrentPhase) || (concurrent_phase_roots == _concurrentPhase));
	if (env->_concurrentScavengerSwitchCount != _concurrentScavengerSwitchCount) {
		Trc_MM_Scavenger_switchConcurrent(env->getLanguageVMThread(), _concurrentPhase, _concurrentScavengerSwitchCount, env->_concurrentScavengerSwitchCount);
		env->_concurrentScavengerSwitchCount = _concurrentScavengerSwitchCount;
		_delegate.switchConcurrentForThread(env);
	}
}

void
MM_Scavenger::triggerConcurrentScavengerTransition(MM_EnvironmentBase *env, MM_AllocateDescription *allocDescription)
{
	/* About to block. A dedicated main GC thread will take over for the duration of STW phase (start or end) */
	_mainGCThread.garbageCollect(env, allocDescription);
	/* STW phase is complete */

	/* count every cycle start and cycle end transition */
	_concurrentScavengerSwitchCount += 1;

	/* Ensure switchConcurrentForThread is invoked for each mutator thread. It will be done indirectly,
	 * first time a thread acquires VM access after exclusive VM access is released, through a VM access hook. */
	GC_OMRVMThreadListIterator threadIterator(_extensions->getOmrVM());
	OMR_VMThread *walkThread = NULL;

	while((walkThread = threadIterator.nextOMRVMThread()) != NULL) {
		MM_EnvironmentStandard *threadEnvironment = MM_EnvironmentStandard::getEnvironment(walkThread);
		if (MUTATOR_THREAD == threadEnvironment->getThreadType()) {
			threadEnvironment->forceOutOfLineVMAccess();
		}
	}

	/* For this thread too directly */
	switchConcurrentForThread(env);
}

void
MM_Scavenger::completeConcurrentCycle(MM_EnvironmentBase *env)
{
	/* this is supposed to be called by an external cycle (for example ConcurrentGC, STW phase)
	 * that is just to be started, but cannot before Scavenger is complete */
	Assert_MM_true(NULL == env->_cycleState);
	if (isConcurrentCycleInProgress()) {
		env->_cycleState = &_cycleState;
		triggerConcurrentScavengerTransition(env, NULL);
		env->_cycleState = NULL;
	}
}

extern "C" {

void
concurrentScavengerAsyncCallbackHandler(OMR_VMThread *omrVMThread)
{
	MM_EnvironmentStandard *env = MM_EnvironmentStandard::getEnvironment(omrVMThread);
	MM_GCExtensionsBase *ext = env->getExtensions();

	if (ext->isConcurrentScavengerInProgress()) {
		ext->scavenger->threadReleaseCaches(env, env, true, false);
	}
}

} /* extern "C" */
#endif