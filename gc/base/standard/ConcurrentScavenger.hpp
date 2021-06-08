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

#if !defined(CONCURRENT_SCAVENGER_HPP_)
#define CONCURRENT_SCAVENGER_HPP_

#include "omrcfg.h"

#include "Scavenger.hpp"

class MM_ConcurrentScavenger : public MM_Scavenger
{
    /*
     * Data members
     */
private:
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

#define IS_CONCURRENT_ENABLED _extensions->isConcurrentScavengerEnabled()

protected:

public:
    /*
     * Function members
     */
private:
MMINLINE bool shouldAbortScanLoop(MM_EnvironmentStandard *env) {
		bool shouldAbort = false;

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
				MM_Scavenger::flushBuffersForGetNextScanCache(env);
			}
		} else {		
			shouldAbort = MM_Scavenger::isBackOutFlagRaised();
		}
		
		return shouldAbort;
	}

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
	/**
	 * Perform partial initialization if Garbage Collection is called earlier then GC Main Thread is activated
	 * @param env Main GC thread.
	 */
	virtual MM_ConcurrentPhaseStatsBase *getConcurrentPhaseStats() { return &_concurrentPhaseStats; }
public:

    virtual bool collectorStartup(MM_GCExtensionsBase* extensions);
	virtual void collectorShutdown(MM_GCExtensionsBase* extensions);

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

    MM_ConcurrentScavenger(MM_EnvironmentBase *env, MM_HeapRegionManager *regionManager) :
        MM_Scavenger(env, regionManager)
        , _mainGCThread(env)
		, _concurrentPhase(concurrent_phase_idle)
		, _currentPhaseConcurrent(false)
		, _concurrentScavengerSwitchCount(0)
		, _shouldYield(false)
		, _concurrentPhaseStats(OMR_GC_CYCLE_TYPE_SCAVENGE)
    {}

};
#endif /* CONCURRENT_SCAVENGER_HPP_ */
