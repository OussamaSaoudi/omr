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

#if !defined(SCAVENGER_HPP_)
#define SCAVENGER_HPP_

#include "omrcfg.h"
#include "modronopt.h"
#include "ModronAssertions.h"

#if defined(OMR_GC_MODRON_SCAVENGER)

#include "omrcomp.h"

#include "CollectionStatisticsStandard.hpp"
#include "Collector.hpp"
#include "ConcurrentPhaseStatsBase.hpp"
#include "CopyScanCacheList.hpp"
#include "CopyScanCacheStandard.hpp"
#include "CycleState.hpp"
#include "GCExtensionsBase.hpp"
#if defined(OMR_GC_CONCURRENT_SCAVENGER)
#include "MainGCThread.hpp"
#endif /* OMR_GC_CONCURRENT_SCAVENGER */
#include "ScavengerDelegate.hpp"

struct J9HookInterface;
class GC_ObjectScanner;
class MM_AllocateDescription;
class MM_CollectorLanguageInterface;
class MM_EnvironmentBase;
class MM_HeapRegionManager;
class MM_MemoryPool;
class MM_MemorySubSpace;
class MM_MemorySubSpaceSemiSpace;
class MM_ParallelDispatcher;
class MM_PhysicalSubArena;
class MM_RSOverflow;
class MM_SublistPool;

struct OMR_VM;

extern "C" {
	void concurrentScavengerAsyncCallbackHandler(OMR_VMThread *omrVMThread);
#if defined(OMR_GC_MODRON_CONCURRENT_MARK)
	void oldToOldReferenceCreated(MM_EnvironmentBase *env, omrobjectptr_t objectPtr);
#endif /* OMR_GC_MODRON_CONCURRENT_MARK */
}

/* create macros to interpret the hot field descriptor */
#define HOTFIELD_SHOULD_ALIGN(descriptor) (0x1 == (0x1 & (descriptor)))
#define HOTFIELD_ALIGNMENT_BIAS(descriptor, heapObjectAlignment) (((descriptor) >> 1) * (heapObjectAlignment))
/* If scavenger dynamicBreadthFirstScanOrdering and alwaysDepthCopyFirstOffset is enabled, always copy the first offset of each object after the object itself is copied */
#define DEFAULT_HOT_FIELD_OFFSET 1

/**
 * @todo Provide class documentation
 * @ingroup GC_Modron_Standard
 */
class MM_Scavenger : public MM_Collector
{
	/*
	 * Data members
	 */
private:
	MM_ScavengerDelegate _delegate;

	const uintptr_t _objectAlignmentInBytes;	/**< Run-time objects alignment in bytes */
	bool _isRememberedSetInOverflowAtTheBeginning; /**< Cached RS Overflow flag at the beginning of the scavenge */

	MM_GCExtensionsBase *_extensions;
	
	MM_ParallelDispatcher *_dispatcher;

	volatile uintptr_t _doneIndex; /**< sequence ID of completeScan loop, which we may have a few during one GC cycle */

	MM_MemorySubSpaceSemiSpace *_activeSubSpace; /**< top level new subspace subject to GC */
	MM_MemorySubSpace *_evacuateMemorySubSpace; /**< cached pointer to evacuate subspace within active subspace */
	MM_MemorySubSpace *_survivorMemorySubSpace; /**< cached pointer to survivor subspace within active subspace */
	MM_MemorySubSpace *_tenureMemorySubSpace;

	void *_evacuateSpaceBase, *_evacuateSpaceTop;	/**< cached base and top heap pointers within evacuate subspace */
	void *_survivorSpaceBase, *_survivorSpaceTop;	/**< cached base and top heap pointers within survivor subspace */

	uintptr_t _tenureMask; /**< A bit mask indicating which generations should be tenured on scavenge. */
	bool _expandFailed;
	bool _failedTenureThresholdReached;
	uintptr_t _failedTenureLargestObject;
	uintptr_t _countSinceForcingGlobalGC;

	bool _expandTenureOnFailedAllocate;
	bool _cachedSemiSpaceResizableFlag;
	uintptr_t _minTenureFailureSize;
	uintptr_t _minSemiSpaceFailureSize;
	uintptr_t _recommendedThreads; /** Number of threads recommended to the dispatcher for the Scavenge task */

	MM_CycleState _cycleState;  /**< Embedded cycle state to be used as the main cycle state for GC activity */
	MM_CollectionStatisticsStandard _collectionStatistics;  /** Common collect stats (memory, time etc.) */

	MM_CopyScanCacheList _scavengeCacheFreeList; /**< pool of unused copy-scan caches */
	MM_CopyScanCacheList _scavengeCacheScanList; /**< scan lists */
	volatile uintptr_t _cachedEntryCount; /**< non-empty scanCacheList count (not the total count of caches in the lists) */
	uintptr_t _cachesPerThread; /**< maximum number of copy and scan caches required per thread at any one time */
	omrthread_monitor_t _scanCacheMonitor; /**< monitor to synchronize threads on scan lists */
	omrthread_monitor_t _freeCacheMonitor; /**< monitor to synchronize threads on free list */
	uintptr_t _waitingCountAliasThreshold; /**< Only alias a copy cache IF the number of threads waiting hasn't reached the threshold*/
	volatile uintptr_t _waitingCount; /**< count of threads waiting  on scan cache queues (blocked via _scanCacheMonitor); threads never wait on _freeCacheMonitor */
	uintptr_t _cacheLineAlignment; /**< The number of bytes per cache line which is used to determine which boundaries in memory represent the beginning of a cache line */
	volatile bool _rescanThreadsForRememberedObjects; /**< Indicates that thread-referenced objects were tenured and threads must be rescanned */

	volatile uintptr_t _backOutDoneIndex; /**< snapshot of _doneIndex, when backOut was detected */

	void *_heapBase;  /**< Cached base pointer of heap */
	void *_heapTop;  /**< Cached top pointer of heap */
	MM_HeapRegionManager *_regionManager;

#if defined(OMR_GC_CONCURRENT_SCAVENGER)
	MM_MainGCThread _mainGCThread; /**< An object which manages the state of the main GC thread */
	
	volatile enum ConcurrentState {
		concurrent_phase_idle,
		concurrent_phase_init,
		concurrent_phase_roots,
		concurrent_phase_scan,
		concurrent_phase_complete
	} _concurrentPhase;
	
	bool _currentPhaseConcurrent;
	
	uint64_t _concurrentScavengerSwitchCount; /**< global counter of cycle start and cycle end transitions */
	volatile bool _shouldYield; /**< Set by the first GC thread that observes that a criteria for yielding is met. Reset only when the concurrent phase is finished. */

	MM_ConcurrentPhaseStatsBase _concurrentPhaseStats;
#endif /* OMR_GC_CONCURRENT_SCAVENGER */

#define IS_CONCURRENT_ENABLED _extensions->isConcurrentScavengerEnabled()

protected:

public:
	OMR_VM *_omrVM;
	
	/*
	 * Function members
	 */
private:
	/**
	 * Flush copy/scan count updates, the threads reference and remembered set caches before waiting in getNextScanCache.
	 * This removes the requirement of a synchronization point after calls to completeScan when
	 * it is followed by reference or remembered set processing.
	 * @param env - current thread environment
	 * @param finalFlush - lets the copy/scan flush know if it's the last thread performing the flush
	 */
	void flushBuffersForGetNextScanCache(MM_EnvironmentStandard *env, bool finalFlush = false);
	
	void saveMainThreadTenureTLHRemainders(MM_EnvironmentStandard *env);
	void restoreMainThreadTenureTLHRemainders(MM_EnvironmentStandard *env);
	
	void setBackOutFlag(MM_EnvironmentBase *env, BackOutState value);
	MMINLINE bool isBackOutFlagRaised() { return _extensions->isScavengerBackOutFlagRaised(); }
	
	/**
	 * Check if concurrent phase of the cycle should yield to an external activity. If so, set the flag so that other GC threads react appropriately
	 */ 
	MMINLINE bool checkAndSetShouldYieldFlag(MM_EnvironmentStandard *env);
	
	/**
	 * Check if top level scan loop should be aborted before the work is done
	 */
	MMINLINE bool shouldAbortScanLoop(MM_EnvironmentStandard *env) {
		bool shouldAbort = false;

#if defined(OMR_GC_CONCURRENT_SCAVENGER)
		if (IS_CONCURRENT_ENABLED) {
			/* Concurrent Scavenger needs to drain the scan queue in last scan loop before aborted handling starts.
			 * It is however fine to leave it populated, if we want to yield in a middle of concurrent phase which aborted,
			 * since there will be at least one scan loop afterwards in complete phase that will drain it. Bottom line,
			 * we don't care about isBackOutFlagRaised when deciding whether to yield.
			 */
					 
			shouldAbort = _shouldYield;
			if (shouldAbort) {
				Assert_MM_true(concurrent_phase_scan == _concurrentPhase);
				/* Since we are aborting the scan loop without synchornizing with other GC threads (before which we flush buffers),
				 * we have to do it now. 
				 * There should be no danger in not synchonizing with other threads, since we can only abort/yield in main scan loop
				 * and not during clearable STW phase, where is a potential danger of entering a scan loop without ensuring all
				 * threads flushed buffers from previous scan loop.
				 */
				flushBuffersForGetNextScanCache(env);
			}
		} else
#endif /* #if defined(OMR_GC_CONCURRENT_SCAVENGER) */
		{		
			shouldAbort = isBackOutFlagRaised();
		}
		
		return shouldAbort;
	}

	/** 
	 * A simple heuristic that projects the need for copy-scan cache size pool, based on heap size that Scavenger operates with)
	 */	
	uintptr_t calculateMaxCacheCount(uintptr_t activeMemorySize);

public:
	/**
	 * Hook callback. Called when a global collect has started
	 */
	static void hookGlobalCollectionStart(J9HookInterface** hook, uintptr_t eventNum, void* eventData, void* userData);

	/**
	 * Hook callback. Called when a global collect has completed
	 */
	static void hookGlobalCollectionComplete(J9HookInterface** hook, uintptr_t eventNum, void* eventData, void* userData);
	
	/**
	 *  This method is called on the start of a global GC.
	 *  @param env the current thread.
	 */
	void globalCollectionStart(MM_EnvironmentBase *env);
	
	/**
	 *  This method is called on the completion of a global GC.
	 *  @param env the current thread.
	 */
	void globalCollectionComplete(MM_EnvironmentBase *env);

	/**
	 * Test backout state and inhibit array splitting once backout starts.
	 * @param env current thread environment
	 * @param objectptr the object to scan
	 * @param objectScannerState points to space for inline allocation of scanner
	 * @param flags scanner flags
	 * @return the object scanner
	 */
	MMINLINE GC_ObjectScanner *getObjectScanner(MM_EnvironmentStandard *env, omrobjectptr_t objectptr, void *objectScannerState, uintptr_t flags);

	uintptr_t calculateCopyScanCacheSizeForWaitingThreads(uintptr_t maxCacheSize, uintptr_t threadCount, uintptr_t waitingThreads);
	uintptr_t calculateCopyScanCacheSizeForQueueLength(uintptr_t maxCacheSize, uintptr_t threadCount, uintptr_t scanCacheCount);
	MMINLINE uintptr_t calculateOptimumCopyScanCacheSize(MM_EnvironmentStandard *env);
	MMINLINE MM_CopyScanCacheStandard *reserveMemoryForAllocateInSemiSpace(MM_EnvironmentStandard *env, omrobjectptr_t objectToEvacuate, uintptr_t objectReserveSizeInBytes);
	MM_CopyScanCacheStandard *reserveMemoryForAllocateInTenureSpace(MM_EnvironmentStandard *env, omrobjectptr_t objectToEvacuate, uintptr_t objectReserveSizeInBytes);

	MM_CopyScanCacheStandard *getNextScanCache(MM_EnvironmentStandard *env);

	/**
	 * Implementation of CopyAndForward for slotObject input format
	 * @param slotObject input field in slotObject format
	 */

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
	template <bool csEnabled>
	bool copyAndForward(MM_EnvironmentStandard *env, GC_SlotObject *slotObject)
	{
		omrobjectptr_t oldSlot = slotObject->readReferenceFromSlot();
		omrobjectptr_t slot = oldSlot;
		bool result = false;
		if (csEnabled) {
			result = copyAndForward<true>(env, &slot);
		} else {
			result = copyAndForward<false>(env, &slot);
		}
#if defined(OMR_GC_CONCURRENT_SCAVENGER)
		if (concurrent_phase_scan == _concurrentPhase) {
			if (oldSlot != slot) {
				slotObject->atomicWriteReferenceToSlot(oldSlot, slot);
			}
		} else
#endif /* OMR_GC_CONCURRENT_SCAVENGER */
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
	template <bool csEnabled>
	bool copyAndForward(MM_EnvironmentStandard *env, volatile omrobjectptr_t *objectPtrIndirect)
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
					omrobjectptr_t destinationObjectPtr = NULL;
#if defined(OMR_GC_CONCURRENT_SCAVENGER)
					if (csEnabled) {
						destinationObjectPtr = copy<true>(env, &forwardHeader);
					} else
#endif /* OMR_GC_CONCURRENT_SCAVENGER */
					{
						destinationObjectPtr = copy<false>(env, &forwardHeader);
					}

					if (NULL == destinationObjectPtr) {
						/* Failure - the scavenger must back out the work it has done. */
						/* raise the alert and return (true - must look like a new object was handled) */
						toReturn = true;
#if defined(OMR_GC_CONCURRENT_SCAVENGER)
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
#endif /* OMR_GC_CONCURRENT_SCAVENGER */
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
	 * Handle the path after a failed attempt to forward an object:
	 * try to reuse or abandon reserved memory for this threads destination object candidate.
	 * Infrequent path, hence not inlined.
	 */
	void forwardingFailed(MM_EnvironmentStandard *env, MM_ForwardedHeader* forwardedHeader, omrobjectptr_t destinationObjectPtr, MM_CopyScanCacheStandard *copyCache);
	
	/**
	 * Handle the path after a succeesful attempt to forward an object:
	 * Update the alloc pointer and update various stats.
	 * Frequent path, hence inlined.
	 */	
	MMINLINE void forwardingSucceeded(MM_EnvironmentStandard *env, MM_CopyScanCacheStandard *copyCache, void *newCacheAlloc, uintptr_t oldObjectAge, uintptr_t objectCopySizeInBytes, uintptr_t objectReserveSizeInBytes);

	template <bool csEnabled>
	omrobjectptr_t copy(MM_EnvironmentStandard *env, MM_ForwardedHeader* forwardedHeader)
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
/*
				uintptr_t spaceAvailableForObject = _activeSubSpace->getMaxSpaceForObjectInEvacuateMemory(forwardedHeader->getObject());
				Assert_GC_true_with_message4(env, objectCopySizeInBytes <= spaceAvailableForObject,
						"Corruption in Evacuate at %p: calculated object size %zu larger then available %zu, Forwarded Header at %p\n",
						forwardedHeader->getObject(), objectCopySizeInBytes, spaceAvailableForObject, forwardedHeader);
*/
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
/*
				uintptr_t spaceAvailableForObject = _activeSubSpace->getMaxSpaceForObjectInEvacuateMemory(forwardedHeader->getObject());
				Assert_GC_true_with_message4(env, objectCopySizeInBytes <= spaceAvailableForObject,
						"Corruption in Evacuate at %p: calculated object size %zu larger then available %zu, Forwarded Header at %p\n",
						forwardedHeader->getObject(), objectCopySizeInBytes, spaceAvailableForObject, forwardedHeader);
*/
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
#if defined(OMR_GC_CONCURRENT_SCAVENGER)
		uintptr_t remainingSizeToCopy = 0;
		uintptr_t initialSizeToCopy = 0;
		bool allowDuplicate = false;
		bool allowDuplicateOrConcurrentDisabled = true;

		if (csEnabled) {
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
		} else
#endif /* OMR_GC_CONCURRENT_SCAVENGER */
		{
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

#if defined(OMR_GC_CONCURRENT_SCAVENGER)
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
#endif /* OMR_GC_CONCURRENT_SCAVENGER */
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

#if defined(OMR_GC_CONCURRENT_SCAVENGER)
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
#endif /* OMR_GC_CONCURRENT_SCAVENGER */
				forwardingSucceeded(env, copyCache, newCacheAlloc, oldObjectAge, objectCopySizeInBytes, objectReserveSizeInBytes);

				/* depth copy the hot fields of an object if scavenger dynamicBreadthFirstScanOrdering is enabled */
				depthCopyHotFields(env, forwardedHeader, destinationObjectPtr);
#if defined(OMR_GC_CONCURRENT_SCAVENGER)
			} else { /* CS build flag  enabled: mid point of nested if-forwarding-succeeded check */

				forwardingFailed(env, forwardedHeader, destinationObjectPtr, copyCache);

			} /* CS build flag  enabled: end of nested if-forwarding-succeeded check */
#endif
		} else { /* CS build flag  enabled: mid point of outter   if-forwarding-succeeded check
				* CS build flag disabled: mid point of the only if-forwarding-succeeded check */

			forwardingFailed(env, forwardedHeader, destinationObjectPtr, copyCache);

		} /* CS build flag  enabled: end of outter   if-forwarding-succeeded check
		* CS build flag disabled: end of the only if-forwarding-succeeded check */

		/* return value for updating the slot */
		return destinationObjectPtr;
	}

	/* Flush remaining Copy Scan updates which would otherwise be discarded 
	 * @param majorFlush last thread to flush updates should perform a major flush (push accumulated updates to history record) 
	 */ 
	MMINLINE void flushCopyScanCounts(MM_EnvironmentBase* env, bool majorFlush);

	/* Depth copy the hot fields of an object.
	 * @param forwardedHeader Forwarded header of an object
	 * @param destinationObjectPtr DestinationObjectPtr of the object described by the forwardedHeader
	 */ 
	MMINLINE void depthCopyHotFields(MM_EnvironmentStandard *env, MM_ForwardedHeader* forwardedHeader, omrobjectptr_t destinationObjectPtr) {
		/* depth copy the hot fields of an object up to a depth specified by depthCopyMax */
		if (env->_hotFieldCopyDepthCount < _extensions->depthCopyMax) {
			uint8_t hotFieldOffset = _extensions->objectModel.getHotFieldOffset(forwardedHeader);
			if (U_8_MAX != hotFieldOffset) {
				copyHotField(env, destinationObjectPtr, hotFieldOffset);
				uint8_t hotFieldOffset2 = _extensions->objectModel.getHotFieldOffset2(forwardedHeader);
				if (U_8_MAX != hotFieldOffset2) {
					copyHotField(env, destinationObjectPtr, hotFieldOffset2);
					uint8_t hotFieldOffset3 = _extensions->objectModel.getHotFieldOffset3(forwardedHeader);
					if (U_8_MAX != hotFieldOffset3) {
						copyHotField(env, destinationObjectPtr, hotFieldOffset3);
					}
				}
			} else if (_extensions->alwaysDepthCopyFirstOffset && !_extensions->objectModel.isIndexable(forwardedHeader)) {
				copyHotField(env, destinationObjectPtr, DEFAULT_HOT_FIELD_OFFSET);
			}
		}
	}
	/* Copy the the hot field of an object.
	 * Valid if scavenger dynamicBreadthScanOrdering is enabled.
	 * @param destinationObjectPtr The object who's hot field will be copied
	 * @param offset The object field offset of the hot field to be copied
	 */ 
	MMINLINE void copyHotField(MM_EnvironmentStandard *env, omrobjectptr_t destinationObjectPtr, uint8_t offset) {
		bool const compressed = _extensions->compressObjectReferences();
		GC_SlotObject hotFieldObject(_omrVM, GC_SlotObject::addToSlotAddress((fomrobject_t*)((uintptr_t)destinationObjectPtr), offset, compressed));
		omrobjectptr_t objectPtr = hotFieldObject.readReferenceFromSlot();
		if (isObjectInEvacuateMemory(objectPtr)) {
			/* Hot field needs to be copy and forwarded.  Check if the work has already been done */
			MM_ForwardedHeader forwardHeaderHotField(objectPtr, compressed);
			if (!forwardHeaderHotField.isForwardedPointer()) {
				env->_hotFieldCopyDepthCount += 1;
				copyObject(env, &forwardHeaderHotField);
				env->_hotFieldCopyDepthCount -= 1;
			}
		}
	}

	MMINLINE void updateCopyScanCounts(MM_EnvironmentBase* env, uint64_t slotsScanned, uint64_t slotsCopied);
	bool splitIndexableObjectScanner(MM_EnvironmentStandard *env, GC_ObjectScanner *objectScanner, uintptr_t startIndex, omrobjectptr_t *rememberedSetSlot);

	/**
	 * Scavenges the contents of an object.
	 * @param env The environment.
	 * @param objectPtr The pointer to the object.
	 * @param scanCache The scan cache for the environment
	 * @param flags A bit map of GC_ObjectScanner::InstanceFlags.
	 * @return Whether or not objectPtr should be remembered.
	 */
	template <bool csEnabled>
	bool scavengeObjectSlots(MM_EnvironmentStandard *env, MM_CopyScanCacheStandard *scanCache, omrobjectptr_t objectPtr, uintptr_t flags, omrobjectptr_t *rememberedSetSlot)
	{
		GC_ObjectScanner *objectScanner = NULL;
		GC_ObjectScannerState objectScannerState;
		/* scanCache will be NULL if called from outside completeScan() */
		if ((NULL == scanCache) || !scanCache->isSplitArray()) {
			/* try to get a new scanner instance from the cli */
			objectScanner = getObjectScanner(env, objectPtr, &objectScannerState, flags);
			if ((NULL == objectScanner) || objectScanner->isLeafObject()) {
				/* Object scanner will be NULL if object not scannable by cli (eg, empty pointer array, primitive array) */
				if (NULL != objectScanner) {
					/* Otherwise this is a leaf object -- contains no reference slots */
					env->_scavengerStats._leafObjectCount += 1;
				}
				return false;
			}
		} else {
			/* use scanner cloned into this split array scan cache */
			objectScanner = scanCache->getObjectScanner();
		}

#if defined(OMR_GC_MODRON_SCAVENGER_STRICT)
		if ((NULL != scanCache) && objectScanner->isIndexableObject()) {
			GC_IndexableObjectScanner *indexableScanner = (GC_IndexableObjectScanner *)objectScanner;
			Assert_MM_true(objectPtr == indexableScanner->getArrayObject());
			Assert_MM_true(scanCache->isSplitArray() && (0 < scanCache->_arraySplitIndex));
			Assert_MM_true(rememberedSetSlot == scanCache->_arraySplitRememberedSlot);
		}
#endif /* defined(OMR_GC_MODRON_SCAVENGER_STRICT) */

		if (objectScanner->isIndexableObject()) {
			/* set scanning bounds for this scanner; if non-empty tail, clone scanner into split array cache and add cache to worklist */
			uintptr_t splitIndex = (NULL != scanCache) ? scanCache->_arraySplitIndex : 0;
			if (!splitIndexableObjectScanner(env, objectScanner, splitIndex, rememberedSetSlot)) {
				/* scan to end of array if can't split */
				((GC_IndexableObjectScanner *)objectScanner)->scanToLimit();
			}
		}

		uint64_t slotsCopied = 0;
		uint64_t slotsScanned = 0;
		bool shouldRemember = false;
		GC_SlotObject *slotObject = NULL;

		MM_CopyScanCacheStandard **copyCache = &(env->_effectiveCopyScanCache);
		while (NULL != (slotObject = objectScanner->getNextSlot())) {
			bool isSlotObjectInNewSpace = false;
			if (csEnabled) {
				isSlotObjectInNewSpace = copyAndForward<true>(env, slotObject);
			} else {
				isSlotObjectInNewSpace = copyAndForward<false>(env, slotObject);
			}
			shouldRemember |= isSlotObjectInNewSpace;
			if (NULL != *copyCache) {
				slotsCopied += 1;
			}
			slotsScanned += 1;
		}
		updateCopyScanCounts(env, slotsScanned, slotsCopied);

		if (shouldRemember && (NULL != rememberedSetSlot)) {
			Assert_MM_true(!isObjectInNewSpace(objectPtr));
			Assert_MM_true(_extensions->objectModel.isRemembered(objectPtr));
			Assert_MM_true(objectPtr == (omrobjectptr_t)((uintptr_t)*rememberedSetSlot & ~(uintptr_t)DEFERRED_RS_REMOVE_FLAG));
			/* Set the remembered set slot to the object pointer in case it was still marked for removal. */
			*rememberedSetSlot = objectPtr;
		}
#if defined(OMR_GC_MODRON_CONCURRENT_MARK)
		bool isParentInNewSpace = isObjectInNewSpace(objectPtr);
		if (_extensions->shouldScavengeNotifyGlobalGCOfOldToOldReference() && csEnabled && !isParentInNewSpace && !shouldRemember) {
			/* Old object that has only references to old objects. If parent object has already been scanned (in Marking sense)
			* since it has been tenured, let Concurrent Marker know it has a newly created old reference, otherwise it may miss to find it. */
			oldToOldReferenceCreated(env, objectPtr);
		}
#endif /* OMR_GC_MODRON_CONCURRENT_MARK */

		return shouldRemember;
	}

	/**
	 * Scans the slots of a non-indexable object, remembering objects as required. Scanning is interrupted
	 * as soon as there is a copy cache that is preferred to the current scan cache. This is returned
	 * in nextScanCache.
	 *
	 * @param scanCache current cache being scanned
	 * @param objectPtr current object being scanned
	 * @param nextScanCache the updated scanCache after re-aliasing.
	 */
	template <bool csEnabled>
	MM_CopyScanCacheStandard * incrementalScavengeObjectSlots(MM_EnvironmentStandard *env, omrobjectptr_t objectPtr, MM_CopyScanCacheStandard *scanCache)
	{
		/* Get an object scanner from the CLI if not resuming from a scan cache that was previously suspended */
		GC_ObjectScanner *objectScanner = NULL;
		if (!scanCache->_hasPartiallyScannedObject) {
			if (!scanCache->isSplitArray()) {
				/* try to get a new scanner instance from the cli */
				objectScanner = getObjectScanner(env, objectPtr, scanCache->getObjectScanner(), GC_ObjectScanner::scanHeap);
				if ((NULL == objectScanner) || objectScanner->isLeafObject()) {
					/* Object scanner will be NULL if object not scannable by cli (eg, empty pointer array, primitive array) */
					if (NULL != objectScanner) {
						/* Otherwise this is a leaf object -- contains no reference slots */
						env->_scavengerStats._leafObjectCount += 1;
					}
					return NULL;
				}
			} else {
				/* reuse scanner cloned into this split array scan cache */
				objectScanner = scanCache->getObjectScanner();
			}
			if (objectScanner->isIndexableObject()) {
				/* set scanning bounds for this scanner; if non-empty tail, add split array cache to worklist and clone this indexableScanner into split cache */
				if (!splitIndexableObjectScanner(env, objectScanner, scanCache->_arraySplitIndex, scanCache->_arraySplitRememberedSlot)) {
					/* scan to end of array if can't split */
					((GC_IndexableObjectScanner *)objectScanner)->scanToLimit();
				}
			}
			scanCache->_shouldBeRemembered = false;
		} else {
			/* resume suspended object scanner */
			objectScanner = scanCache->getObjectScanner();
		}

#if defined(OMR_GC_MODRON_SCAVENGER_STRICT)
		if (scanCache->isSplitArray()) {
			GC_IndexableObjectScanner *indexableScanner = (GC_IndexableObjectScanner *)objectScanner;
			Assert_MM_true(objectScanner->isIndexableObject());
			Assert_MM_true(objectPtr == indexableScanner->getArrayObject());
			Assert_MM_true(0 < scanCache->_arraySplitIndex);
		} else {
			Assert_MM_true(0 == scanCache->_arraySplitIndex);
			Assert_MM_true(NULL == scanCache->_arraySplitRememberedSlot);
		}
#endif /* defined(OMR_GC_MODRON_SCAVENGER_STRICT) */

		GC_SlotObject *slotObject;
		uint64_t slotsCopied = 0;
		uint64_t slotsScanned = 0;

		while (NULL != (slotObject = objectScanner->getNextSlot())) {
			/* If the object should be remembered and it is in old space, remember it */
			bool isSlotObjectInNewSpace = false;
			if (csEnabled) {
				isSlotObjectInNewSpace = copyAndForward<true>(env, slotObject);
			} else {
				isSlotObjectInNewSpace = copyAndForward<false>(env, slotObject);
			}
			scanCache->_shouldBeRemembered |= isSlotObjectInNewSpace;
			slotsScanned += 1;

			MM_CopyScanCacheStandard *copyCache = env->_effectiveCopyScanCache;
			if (NULL != copyCache) {
				/* Copy cache will be set only if a referent object is copied (ie, if not previously forwarded) */
				slotsCopied += 1;

				MM_CopyScanCacheStandard *nextScanCache = aliasToCopyCache(env, slotObject, scanCache, copyCache);
				if (NULL != nextScanCache) {
					/* alias and switch to nextScanCache if it was selected */
					updateCopyScanCounts(env, slotsScanned, slotsCopied);
					return nextScanCache;
				}
			}
		}
		updateCopyScanCounts(env, slotsScanned, slotsCopied);

		scanCache->_hasPartiallyScannedObject = false;
		if (scanCache->_shouldBeRemembered) {
			if (NULL != scanCache->_arraySplitRememberedSlot) {
				Assert_MM_true(!isObjectInNewSpace(objectPtr));
				Assert_MM_true(_extensions->objectModel.isRemembered(objectPtr));
				Assert_MM_true(objectPtr == (omrobjectptr_t)((uintptr_t)*(scanCache->_arraySplitRememberedSlot) & ~(uintptr_t)DEFERRED_RS_REMOVE_FLAG));
				/* Set the remembered set slot to the object pointer in case it was still marked for removal. */
				*(scanCache->_arraySplitRememberedSlot) = objectPtr;
			} else {
				rememberObject(env, objectPtr);
			}
			scanCache->_shouldBeRemembered = false;
		}

#if defined(OMR_GC_MODRON_CONCURRENT_MARK)
		bool isParentInNewSpace = isObjectInNewSpace(objectPtr);
		if (_extensions->shouldScavengeNotifyGlobalGCOfOldToOldReference() && csEnabled && !isParentInNewSpace && !scanCache->_shouldBeRemembered) {
			/* Old object that has only references to old objects. If parent object has already been scanned (in Marking sense)
			* since it has been tenured, let Concurrent Marker know it has a newly created old reference, otherwise it may miss to find it. */
			oldToOldReferenceCreated(env, objectPtr);
		}
#endif /* OMR_GC_MODRON_CONCURRENT_MARK */

		return NULL;
	}

	/**
	 * For fast traversal of deep structure nodes - scan objects with self referencing fields with priority
	 * Split into two functions deepScan and deepScanOutline. Frequently called checks (see lazy start check) must be inlined
	 * @param env The environment.
	 * @param objectPtr The pointer to the object.
	 * @param priorityFieldOffset1 Offset to the first priority field of the object
	 * @param priorityFieldOffset2 Offset to the second priority field, if it can't follow through in one direction, 
	 * it will attempt to use the second self referencing field 
	 */
	MMINLINE void
	deepScan(MM_EnvironmentStandard *env, omrobjectptr_t objectPtr, uintptr_t priorityFieldOffset1, uintptr_t priorityFieldOffset2)
	{
		/**
		* Inhibit the special treatment routine with relatively high probability to skip over most  
		* false positives (shorter lists), while only marginally delay detection of very deep structures.
		*/	
		if (shouldStartDeepScan(env, objectPtr)) {
			deepScanOutline(env, objectPtr, priorityFieldOffset1, priorityFieldOffset2);
		}
	}
	
	/**
	 * Deep scan lazy start check - condition used for gatekeeping
	 * @param env The environment.
	 * @param objectPtr The pointer to the object.
	 * @return True If deep scan should start
	 */
	MMINLINE bool
	shouldStartDeepScan(MM_EnvironmentStandard *env, omrobjectptr_t objectPtr)
	{
		/* Check last few LSB of the object address for probability 1/16 */
		return (0 == ((uintptr_t)objectPtr & 0x78)); 
	}

	void deepScanOutline(MM_EnvironmentStandard *env, omrobjectptr_t objectPtr, uintptr_t priorityFieldOffset1, uintptr_t priorityFieldOffset2);

	MMINLINE bool scavengeRememberedObject(MM_EnvironmentStandard *env, omrobjectptr_t objectPtr);
	void scavengeRememberedSetList(MM_EnvironmentStandard *env);
	void scavengeRememberedSetOverflow(MM_EnvironmentStandard *env);
	MMINLINE void flushRememberedSet(MM_EnvironmentStandard *env);
	void pruneRememberedSetList(MM_EnvironmentStandard *env);
	void pruneRememberedSetOverflow(MM_EnvironmentStandard *env);

	/**
	 * Checks if the  Object should be remembered or not
	 * @param env Standard Environment
	 * @param objectPtr The pointer to the  Object in Tenured Space.
	 * @return True If Object should be remembered
	 */
	bool shouldRememberObject(MM_EnvironmentStandard *env, omrobjectptr_t objectPtr);

	/**
	 * BackOutFixSlot implementation
	 * @param slotObject input field in slotObject format
	 */
	bool backOutFixSlot(GC_SlotObject *slotObject);

	void backoutFixupAndReverseForwardPointersInSurvivor(MM_EnvironmentStandard *env);
	void processRememberedSetInBackout(MM_EnvironmentStandard *env);
	void completeBackOut(MM_EnvironmentStandard *env);

#if defined(OMR_GC_CONCURRENT_SCAVENGER)
	void fixupNurserySlots(MM_EnvironmentStandard *env);
	void fixupObjectScan(MM_EnvironmentStandard *env, omrobjectptr_t objectPtr);
	bool fixupSlot(GC_SlotObject *slotObject);
	bool fixupSlotWithoutCompression(volatile omrobjectptr_t *slotPtr);
	
	void scavengeRememberedSetListIndirect(MM_EnvironmentStandard *env);
	void scavengeRememberedSetListDirect(MM_EnvironmentStandard *env);

	MMINLINE void flushInactiveSurvivorCopyScanCache(MM_EnvironmentStandard *currentEnv, MM_EnvironmentStandard *targetEnv, bool flushCaches, bool final);
	MMINLINE void deactivateSurvivorCopyScanCache(MM_EnvironmentStandard *currentEnv, MM_EnvironmentStandard *targetEnv, bool flushCaches, bool final);
	MMINLINE void flushInactiveTenureCopyScanCache(MM_EnvironmentStandard *currentEnv, MM_EnvironmentStandard *targetEnv, bool flushCaches, bool final);
	MMINLINE void deactivateTenureCopyScanCache(MM_EnvironmentStandard *currentEnv, MM_EnvironmentStandard *targetEnv, bool flushCaches, bool final);
	MMINLINE void flushInactiveDeferredCopyScanCache(MM_EnvironmentStandard *currentEnv, MM_EnvironmentStandard *targetEnv, bool flushCaches, bool final);
	MMINLINE void deactivateDeferredCopyScanCache(MM_EnvironmentStandard *currentEnv, MM_EnvironmentStandard *targetEnv, bool flushCaches, bool final);
#endif /* OMR_GC_CONCURRENT_SCAVENGER */

	/**
 	 * Request for percolate GC
 	 * 
 	 * @return true if Global GC was executed, false if concurrent kickoff forced or Global GC is not possible 
 	 */
	bool percolateGarbageCollect(MM_EnvironmentBase *env, MM_MemorySubSpace *subSpace, MM_AllocateDescription *allocDescription, PercolateReason percolateReason, uint32_t gcCode);
	
	void reportGCCycleStart(MM_EnvironmentStandard *env);
	void reportGCCycleEnd(MM_EnvironmentStandard *env);
	void reportGCCycleFinalIncrementEnding(MM_EnvironmentStandard *envModron);

	MMINLINE void clearExpandFailedFlag() { _expandFailed = false; };
	MMINLINE void setExpandFailedFlag() { _expandFailed = true; };
	MMINLINE bool expandFailed() { return _expandFailed; };

	MMINLINE void clearFailedTenureThresholdFlag() { _failedTenureThresholdReached = false; };
	MMINLINE void setFailedTenureThresholdFlag() { _failedTenureThresholdReached = true; };
	MMINLINE void setFailedTenureLargestObject(uintptr_t size) { _failedTenureLargestObject = size; };
	MMINLINE uintptr_t getFailedTenureLargestObject() { return _failedTenureLargestObject; };
	MMINLINE bool failedTenureThresholdReached() { return _failedTenureThresholdReached; };

	void completeScanCache(MM_EnvironmentStandard *env, MM_CopyScanCacheStandard* scanCache);
	void incrementalScanCacheBySlot(MM_EnvironmentStandard *env, MM_CopyScanCacheStandard* scanCache);

	MMINLINE MM_CopyScanCacheStandard *aliasToCopyCache(MM_EnvironmentStandard *env, GC_SlotObject *scannedSlot, MM_CopyScanCacheStandard* scanCache, MM_CopyScanCacheStandard* copyCache);
	MMINLINE uintptr_t scanCacheDistanceMetric(MM_CopyScanCacheStandard* cache, GC_SlotObject *scanSlot);
	MMINLINE uintptr_t copyCacheDistanceMetric(MM_CopyScanCacheStandard* cache);

	MMINLINE MM_CopyScanCacheStandard *getNextScanCacheFromList(MM_EnvironmentStandard *env);
	/**
	 * Called at the end of a task to return empty caches to the global free pool
	 */
	void finalReturnCopyCachesToFreeList(MM_EnvironmentStandard *env);
	/* 
	 * Used by CS to return empty caches during intermediate blocks, to aid with more precise counting of free/empty cache in the pool
	 */
	void returnEmptyCopyCachesToFreeList(MM_EnvironmentStandard *env);
	MMINLINE void addCacheEntryToScanListAndNotify(MM_EnvironmentStandard *env, MM_CopyScanCacheStandard *newCacheEntry);

	MMINLINE bool
	isWorkAvailableInCacheWithCheck(MM_CopyScanCacheStandard *cache)
	{
		return ((NULL != cache) && cache->isScanWorkAvailable());
	}

	MMINLINE bool
	isEmptyCacheWithCheck(MM_CopyScanCacheStandard *cache)
	{
		return ((NULL != cache) && !cache->isScanWorkAvailable());
	}

	/**
	 * An attempt to get a preallocated scan cache header, free list will be locked
	 * @param env - current thread environment
	 * @return pointer to scan cache header or NULL if attempt fail
	 */
	MMINLINE MM_CopyScanCacheStandard *getFreeCache(MM_EnvironmentStandard *env);

	/**
	 * An attempt to create chunk of scan cache headers in heap
	 * @param env - current thread environment
	 * @return pointer to allocated chunk of scan cache headers or NULL if attempt fail
	 */
	MM_CopyScanCacheStandard *createCacheInHeap(MM_EnvironmentStandard *env);

	/**
	 * Return cache back to free list if it is not used for copy.
	 * Clear cache if it has not been cleared yet
	 * @param env - current thread environment
	 * @param cache cache to be flushed
	 */
	void flushCache(MM_EnvironmentStandard *env, MM_CopyScanCacheStandard *cache);

	/**
	 * Release local Copy cache
	 * Put cache to scanned list if it has not been scanned and has scan work to do
	 * @param env - current thread environment
	 * @param cache cache to be flushed
	 * @return cache to reuse, if any
	 */
	MM_CopyScanCacheStandard *releaseLocalCopyCache(MM_EnvironmentStandard *env, MM_CopyScanCacheStandard *cache);

	/**
	 * Clear cache
	 * Return memory has not been allocated to memory pool
	 * @param env - current thread environment
	 * @param cache cache to be flushed
	 * @return true if sufficent amount of memory is left in the cache to create a 'remainder' for later usage
	 */
	bool clearCache(MM_EnvironmentStandard *env, MM_CopyScanCacheStandard *cache);

	/**
	 * Called (typically at the end of GC) to explicitly abandon the TLH remainders (for the calling thread)
	 */
	void abandonSurvivorTLHRemainder(MM_EnvironmentStandard *env);
	void abandonTenureTLHRemainder(MM_EnvironmentStandard *env, bool preserveRemainders = false);
	
	MMINLINE bool activateSurvivorCopyScanCache(MM_EnvironmentStandard *env);
	MMINLINE bool activateTenureCopyScanCache(MM_EnvironmentStandard *env);
	void activateDeferredCopyScanCache(MM_EnvironmentStandard *env);

	void reportGCStart(MM_EnvironmentStandard *env);
	void reportGCEnd(MM_EnvironmentStandard *env);
	void reportGCIncrementStart(MM_EnvironmentStandard *env);
	void reportGCIncrementEnd(MM_EnvironmentStandard *env);
	void reportScavengeStart(MM_EnvironmentStandard *env);
	void reportScavengeEnd(MM_EnvironmentStandard *env, bool lastIncrement);

	/**
	 * Add the specified object to the remembered set.
	 * Grow the remembered set if necessary and, if that fails, overflow.
	 * If the object is already remembered or is in new space, do nothing.
	 *
	 * @param env[in] the current thread
	 * @param objectPtr[in] the object to remember
	 */
	void rememberObject(MM_EnvironmentStandard *env, omrobjectptr_t objectPtr);

	/*
	 * Scan Tenure and add all found Remembered objects to Overflow
	 * @param env - Environment
	 * @param overflow - pointer to RS Overflow
	 */
	void addAllRememberedObjectsToOverflow(MM_EnvironmentStandard *env, MM_RSOverflow *overflow);

	void clearRememberedSetLists(MM_EnvironmentStandard *env);

	MMINLINE bool isRememberedSetInOverflowState() { return _extensions->isRememberedSetInOverflowState(); }
	MMINLINE void setRememberedSetOverflowState() { _extensions->setRememberedSetOverflowState(); }
	MMINLINE void clearRememberedSetOverflowState() { _extensions->clearRememberedSetOverflowState(); }

	/* Auto-remember stack objects so JIT can omit generational barriers */
	void rescanThreadSlots(MM_EnvironmentStandard *env);
	/**
	 * Determine if the specified remembered object was referenced by a thread or stack.
	 *
	 * @param env[in] the current thread
	 * @param objectPtr[in] a remembered object
	 *
	 * @return true if the object is a remembered thread reference
	 */
	bool isRememberedThreadReference(MM_EnvironmentStandard *env, omrobjectptr_t objectPtr);

	/**
	 * Determine if the specified remembered object was referenced by a thread or stack and process it
	 *
	 * @param env[in] the current thread
	 * @param objectPtr[in] a remembered object
	 *
	 * @return true if the object is a remembered thread reference
	 */
	bool processRememberedThreadReference(MM_EnvironmentStandard *env, omrobjectptr_t objectPtr);

	/**
	 * Clear global (not thread local) stats for current phase/increment
	 * @param firstIncrement true if first increment in a cycle
	 */
	void clearIncrementGCStats(MM_EnvironmentBase *env, bool firstIncrement);
	/**
	 * Clear global (not thread local) cumulative cycle stats 
	 */
	void clearCycleGCStats(MM_EnvironmentBase *env);
	/**
	 * Clear thread local stats for current phase/increment
	 * @param firstIncrement true if first increment in a cycle
	 */
	void clearThreadGCStats(MM_EnvironmentBase *env, bool firstIncrement);
	/**
	 * Merge thread local stats for current phase/increment in to global current increment stats
	 */	
	void mergeThreadGCStats(MM_EnvironmentBase *env);
	/**
	 * Merge global current increment stats in to global cycle stats
	 * @param firstIncrement true if last increment in a cycle
	 */		
	void mergeIncrementGCStats(MM_EnvironmentBase *env, bool lastIncrement);
	/**
	 * Common merge logic used for both thread and increment level merges.
	 */
	void mergeGCStatsBase(MM_EnvironmentBase *env, MM_ScavengerStats *finalGCStats, MM_ScavengerStats *scavStats);
	bool canCalcGCStats(MM_EnvironmentStandard *env);
	void calcGCStats(MM_EnvironmentStandard *env);

	/**
	 * The implementation of Adaptive Threading. This routine is called at the
	 * end of each successful scavenge to determine the optimal number of threads for
	 * the subsequent cycle. This is based on the completed cycle's stall/busy stats (adaptive model).
	 * This function set's _recommendedThreads, which in turn get's used when dispatching
	 * the next cycle's scavege task.
	 */
	void calculateRecommendedWorkingThreads(MM_EnvironmentStandard *env);

	void scavenge(MM_EnvironmentBase *env);
	bool scavengeCompletedSuccessfully(MM_EnvironmentStandard *env);
	virtual	void mainThreadGarbageCollect(MM_EnvironmentBase *env, MM_AllocateDescription *allocDescription, bool initMarkMap = false, bool rebuildMarkBits = false);

	MMINLINE uintptr_t
	isTiltedScavenge()
	{
		return _extensions->tiltedScavenge ? 1 : 0;
	}

	/**
	 * Calculate tilt ratio
	 * @return tiltRatio
	 */
	uintptr_t calculateTiltRatio();

	/**
	 * The implementation of the Lookback scavenger tenure strategy.
	 * This strategy would, for each object age, check the survival history of
	 * that generation of objects (a diagonal down-left check in the survival
	 * history). If, historically, the survival rate of that generation of
	 * objects is always above minimumSurvivalRate, that age will be set for
	 * tenure this scavenge.
	 * @param minimumSurvivalRate The minimum survival rate required to consider tenuring.
	 * @return A tenure mask for the resulting ages to tenure.
	 */
	uintptr_t calculateTenureMaskUsingLookback(double minimumSurvivalRate);

	/**
	 * The implementation of the History scavenger tenure strategy.
	 * This strategy would, for each object age, check the survival history of
	 * that age (a vertical check in the survival history). If, historically,
	 * the survival rate of that age is always above minimumSurvivalRate, that
	 * age will be set for tenure this scavenge.
	 * @param minimumSurvivalRate The minimum survival rate required to consider tenuring.
	 * @return A tenure mask for the resulting ages to tenure.
	 */
	uintptr_t calculateTenureMaskUsingHistory(double minimumSurvivalRate);

	/**
	 * The implementation of the Fixed scavenger tenure strategy.
	 * This strategy will tenure any object who's age is above or equal to
	 * tenureAge.
	 * @param tenureAge The tenure age that objects should be tenured at.
	 * @return A tenure mask for the resulting ages to tenure.
	 */
	uintptr_t calculateTenureMaskUsingFixed(uintptr_t tenureAge);

	/**
	 * Calculates which generations should be tenured in the form of a bit mask.
	 * @return mask of ages to tenure
	 */
	uintptr_t calculateTenureMask();

	/**
	 * reset LargeAllocateStats in Tenure Space
	 * @param env Main GC thread.
	 */
	void resetTenureLargeAllocateStats(MM_EnvironmentBase *env);

	/* API used by ParallelScavengeTask to set _waitingCountAliasThreshold. */
	void setAliasThreshold(uintptr_t waitingCountAliasThreshold) { _waitingCountAliasThreshold = waitingCountAliasThreshold; }
	
	/**
	 * Notify Collector that a thread is about to acquire Exclusive VM access.
	 * This can be useful in scenario when GC is concurrent, and currently in progress.
	 * env invoking thread that is about to acquire Exclusive VM access
	 */
	void externalNotifyToYield(MM_EnvironmentBase* env);
	
	/**
	 * For CS, last thread to block, before notifying other threads to unblock 
	 * will check if all caches are returned to the free global pool. If not,
	 * it will activate Async Handler to force mutators to flush caches, 
	 * and go back to scanning and eventually getting to this point again.
	 */
	bool shouldDoFinalNotify(MM_EnvironmentStandard *env);

protected:
	virtual void setupForGC(MM_EnvironmentBase *env);
	virtual void mainSetupForGC(MM_EnvironmentStandard *env);
	virtual void workerSetupForGC(MM_EnvironmentStandard *env);

	virtual bool initialize(MM_EnvironmentBase *env);
	virtual void tearDown(MM_EnvironmentBase *env);

	virtual bool internalGarbageCollect(MM_EnvironmentBase *env, MM_MemorySubSpace *subSpace, MM_AllocateDescription *allocDescription);
	virtual void internalPreCollect(MM_EnvironmentBase *env, MM_MemorySubSpace *subSpace, MM_AllocateDescription *allocDescription, uint32_t gcCode);
	virtual void internalPostCollect(MM_EnvironmentBase *env, MM_MemorySubSpace *subSpace);

	/**
	 * process LargeAllocateStats before GC
	 * merge largeObjectAllocateStats in nursery space(no averaging)
	 * @param env Main GC thread.
	 */
	virtual void processLargeAllocateStatsBeforeGC(MM_EnvironmentBase *env);

	/**
	 * process LargeAllocateStats after GC
	 * merge and average largeObjectAllocateStats in tenure space
	 * merge FreeEntry AllocateStats in tenure space
	 * estimate Fragmentation
	 * @param env Main GC thread.
	 */
	virtual void processLargeAllocateStatsAfterGC(MM_EnvironmentBase *env);

#if defined(OMR_GC_CONCURRENT_SCAVENGER)
	/**
	 * Perform partial initialization if Garbage Collection is called earlier then GC Main Thread is activated
	 * @param env Main GC thread.
	 */
	virtual MM_ConcurrentPhaseStatsBase *getConcurrentPhaseStats() { return &_concurrentPhaseStats; }
#endif /* OMR_GC_CONCURRENT_SCAVENGER */
	
public:

	static MM_Scavenger *newInstance(MM_EnvironmentStandard *env, MM_HeapRegionManager *regionManager);
	virtual void kill(MM_EnvironmentBase *env);

	MM_ScavengerDelegate* getDelegate() { return &_delegate; }

	/* Read Barrier Verifier specific methods */
#if defined(OMR_ENV_DATA64) && defined(OMR_GC_FULL_POINTERS)
	virtual void scavenger_poisonSlots(MM_EnvironmentBase *env);
	virtual void scavenger_healSlots(MM_EnvironmentBase *env);
#endif /* defined(OMR_ENV_DATA64) && defined(OMR_GC_FULL_POINTERS) */

	virtual bool collectorStartup(MM_GCExtensionsBase* extensions);
	virtual void collectorShutdown(MM_GCExtensionsBase* extensions);

#if defined(OMR_GC_CONCURRENT_SCAVENGER)
	/* API for interaction with MainGCTread */
	virtual bool isConcurrentWorkAvailable(MM_EnvironmentBase *env);
	virtual void preConcurrentInitializeStatsAndReport(MM_EnvironmentBase *env, MM_ConcurrentPhaseStatsBase *stats);
	virtual uintptr_t mainThreadConcurrentCollect(MM_EnvironmentBase *env);
	virtual void postConcurrentUpdateStatsAndReport(MM_EnvironmentBase *env, MM_ConcurrentPhaseStatsBase *stats, UDATA bytesConcurrentlyScanned);

	/* main thread specific methods */
	bool scavengeIncremental(MM_EnvironmentBase *env);
	bool scavengeInit(MM_EnvironmentBase *env);
	bool scavengeRoots(MM_EnvironmentBase *env);
	bool scavengeScan(MM_EnvironmentBase *env);
	bool scavengeComplete(MM_EnvironmentBase *env);
	
	/* mutator thread specific methods */
	void mutatorSetupForGC(MM_EnvironmentBase *env);
	
	/* methods used by either mutator or GC threads */
	/**
	 * All open copy caches (even if not full) are pushed onto scan queue. Unused memory is abondoned.
	 * @param currentEnvBase Current thread in which context this is invoked from. Could be either GC or mutator thread.
	 * @param targetEnvBase  Thread for which copy caches are to be released. Could be either GC or mutator thread.
	 * @param flushCaches If true, really push caches to scan queue, otherwise just deactivate them for possible near future use
	 * @param final If true (typically at the end of a cycle), abandon TLH remainders, too. Otherwise keep them for possible future copy cache refresh.
	 */
	void threadReleaseCaches(MM_EnvironmentBase *currentEnvBase, MM_EnvironmentBase *targetEnvBase, bool flushCaches, bool final);
	
	/**
	 * trigger STW phase (either start or end) of a Concurrent Scavenger Cycle 
	 */ 
	void triggerConcurrentScavengerTransition(MM_EnvironmentBase *envBase, MM_AllocateDescription *allocDescription);
	/**
	 * complete (trigger end) of a Concurrent Scavenger Cycle
	 */
	void completeConcurrentCycle(MM_EnvironmentBase *envBase);

	/* worker thread */
	void workThreadProcessRoots(MM_EnvironmentStandard *env);
	void workThreadScan(MM_EnvironmentStandard *env);
	void workThreadComplete(MM_EnvironmentStandard *env);

	/**
	 * GC threads may call it to determine if running in a context of 
	 * concurrent or STW task
	 */
	bool isCurrentPhaseConcurrent() {
		return _currentPhaseConcurrent;
	}
	
	/**
	 * True if CS cycle is active at any point (STW or concurrent task active,
	 * or even short gaps between STW and concurrent tasks)
	 */
	bool isConcurrentCycleInProgress() {
		return concurrent_phase_idle != _concurrentPhase;
	}
	
	bool isMutatorThreadInSyncWithCycle(MM_EnvironmentBase *env) {
		return (env->_concurrentScavengerSwitchCount == _concurrentScavengerSwitchCount);
	}

	/**
	 * Enabled/disable approriate thread local resources when starting or finishing Concurrent Scavenger Cycle
	 */ 
	void switchConcurrentForThread(MM_EnvironmentBase *env);	
	
	void reportConcurrentScavengeStart(MM_EnvironmentStandard *env);
	void reportConcurrentScavengeEnd(MM_EnvironmentStandard *env);
	
#endif /* OMR_GC_CONCURRENT_SCAVENGER */

	/**
	 * Determine whether the object pointer is found within the heap proper.
	 * @return Boolean indicating if the object pointer is within the heap boundaries.
	 */
	MMINLINE bool
	isHeapObject(omrobjectptr_t objectPtr)
	{
		return ((_heapBase <= (uint8_t *)objectPtr) && (_heapTop > (uint8_t *)objectPtr));
	}

	MMINLINE bool
	isObjectInNewSpace(omrobjectptr_t objectPtr)
	{
		return ((void *)objectPtr >= _survivorSpaceBase) && ((void *)objectPtr < _survivorSpaceTop);
	}

	MMINLINE bool
	isObjectInEvacuateMemory(omrobjectptr_t objectPtr)
	{
		/* check if the object in cached allocate (from GC perspective, evacuate) ranges */
		return ((void *)objectPtr >= _evacuateSpaceBase) && ((void *)objectPtr < _evacuateSpaceTop);
	}
	
	MMINLINE void *
	getEvacuateBase()
	{
		return _evacuateSpaceBase;
	}
	
	MMINLINE void *
	getEvacuateTop()
	{
		return _evacuateSpaceTop;
	}
	
	MMINLINE void *
	getSurvivorBase()
	{
		return _survivorSpaceBase;
	}
	
	MMINLINE void *
	getSurvivorTop()
	{
		return _survivorSpaceTop;
	}

	void workThreadGarbageCollect(MM_EnvironmentStandard *env);

	void scavengeRememberedSet(MM_EnvironmentStandard *env);

	void pruneRememberedSet(MM_EnvironmentStandard *env);

	virtual uintptr_t getVMStateID();

	bool completeScan(MM_EnvironmentStandard *env);

	/**
	 * Attempt to add the specified object to the current thread's remembered set fragment.
	 * Grow the remembered set if necessary and, if that fails, overflow.
	 * The object must already have its remembered bits set.
	 *
	 * @param env[in] the current thread
	 * @param objectPtr[in] the object to remember
	 */
	void addToRememberedSetFragment(MM_EnvironmentStandard *env, omrobjectptr_t objectPtr);

	/**
	 * Provide public (out-of-line) access to private (inline) copyAndForward(), copy() for client language
	 * runtime. Slot holding reference will be updated with new address for referent on return.
	 * @param[in] env Environment pointer for calling thread
	 * @param[in/out] slotPtr Pointer to slot holding reference to object to be copied and forwarded
	 */
	bool copyObjectSlot(MM_EnvironmentStandard *env, volatile omrobjectptr_t *slotPtr);
	bool copyObjectSlot(MM_EnvironmentStandard *env, GC_SlotObject* slotObject);
	omrobjectptr_t copyObject(MM_EnvironmentStandard *env, MM_ForwardedHeader* forwardedHeader);

	/**
	 * Update the given slot to point at the new location of the object, after copying
	 * the object if it was not already.
	 * Attempt to copy (either flip or tenure) the object and install a forwarding
	 * pointer at the new location. The object may have already been copied. In
	 * either case, update the slot to point at the new location of the object.
	 *
	 * @param env[in] the current thread
	 * @param objectPtrIndirect[in/out] the thread or stack slot to be scavenged
	 */
	void copyAndForwardThreadSlot(MM_EnvironmentStandard *env, omrobjectptr_t *objectPtrIndirect);

	/**
	 * This function is called at the end of scavenging if any stack- (or thread-) referenced
	 * objects were tenured during the scavenge. It is called by the RootScanner on each thread
	 * or stack slot.
	 *
	 * @param env[in] the current thread
	 * @param objectPtrIndirect[in] the slot to process
	 */
	void rescanThreadSlot(MM_EnvironmentStandard *env, omrobjectptr_t *objectPtrIndirect);

	uintptr_t getArraySplitAmount(MM_EnvironmentStandard *env, uintptr_t sizeInElements);

	void backOutObjectScan(MM_EnvironmentStandard *env, omrobjectptr_t objectPtr);
	/**
	 * BackOutFixSlotWithoutCompression implementation
	 * @param slotPrt input slot
	 */
	bool backOutFixSlotWithoutCompression(volatile omrobjectptr_t *slotPtr);
	virtual void *createSweepPoolState(MM_EnvironmentBase *env, MM_MemoryPool *memoryPool);
	virtual void deleteSweepPoolState(MM_EnvironmentBase *env, void *sweepPoolState);

	virtual bool heapAddRange(MM_EnvironmentBase *env, MM_MemorySubSpace *subspace, uintptr_t size, void *lowAddress, void *highAddress);
	virtual bool heapRemoveRange(MM_EnvironmentBase *env, MM_MemorySubSpace *subspace, uintptr_t size, void *lowAddress, void *highAddress, void *lowValidAddress, void *highValidAddress);

	virtual void collectorExpanded(MM_EnvironmentBase *env, MM_MemorySubSpace *subSpace, uintptr_t expandSize);
	virtual bool canCollectorExpand(MM_EnvironmentBase *env, MM_MemorySubSpace *subSpace, uintptr_t expandSize);
	virtual uintptr_t getCollectorExpandSize(MM_EnvironmentBase *env);

	MM_Scavenger(MM_EnvironmentBase *env, MM_HeapRegionManager *regionManager) :
		MM_Collector()
		, _delegate(env)
		, _objectAlignmentInBytes(env->getObjectAlignmentInBytes())
		, _isRememberedSetInOverflowAtTheBeginning(false)
		, _extensions(env->getExtensions())
		, _dispatcher(_extensions->dispatcher)
		, _doneIndex(0)
		, _activeSubSpace(NULL)
		, _evacuateMemorySubSpace(NULL)
		, _survivorMemorySubSpace(NULL)
		, _tenureMemorySubSpace(NULL)
		, _evacuateSpaceBase(NULL)
		, _evacuateSpaceTop(NULL)
		, _survivorSpaceBase(NULL)
		, _survivorSpaceTop(NULL)
		, _tenureMask(0)
		, _expandFailed(false)
		, _failedTenureThresholdReached(false)
		, _countSinceForcingGlobalGC(0)
		, _expandTenureOnFailedAllocate(true)
		, _minTenureFailureSize(UDATA_MAX)
		, _minSemiSpaceFailureSize(UDATA_MAX)
		, _recommendedThreads(UDATA_MAX)
		, _cycleState()
		, _collectionStatistics()
		, _cachedEntryCount(0)
		, _cachesPerThread(0)
		, _scanCacheMonitor(NULL)
		, _freeCacheMonitor(NULL)
		, _waitingCountAliasThreshold(0)
		, _waitingCount(0)
		, _cacheLineAlignment(0)
#if !defined(OMR_GC_CONCURRENT_SCAVENGER)
		, _rescanThreadsForRememberedObjects(false)
#endif
		, _backOutDoneIndex(0)
		, _heapBase(NULL)
		, _heapTop(NULL)
		, _regionManager(regionManager)
#if defined(OMR_GC_CONCURRENT_SCAVENGER)
		, _mainGCThread(env)
		, _concurrentPhase(concurrent_phase_idle)
		, _currentPhaseConcurrent(false)
		, _concurrentScavengerSwitchCount(0)
		, _shouldYield(false)
		, _concurrentPhaseStats()
#endif /* #if defined(OMR_GC_CONCURRENT_SCAVENGER) */

		, _omrVM(env->getOmrVM())
	{
		_typeId = __FUNCTION__;
		_cycleType = OMR_GC_CYCLE_TYPE_SCAVENGE;
	}
};

#endif /* OMR_GC_MODRON_SCAVENGER */
#endif /* SCAVENGER_HPP_ */
