#ifndef ZEN_DISTANCE_FIELD_SCHEDULE
#define ZEN_DISTANCE_FIELD_SCHEDULE

// Each work dispatch is followed by an ordered scheduler dispatch, which advances
// that work and prepares the next slice. No phase advances while one of its
// workgroups is still running; the graph's last scheduler only advances.
#ifdef FIELD_PREPARE_SCHEDULE
void FieldPrepareSchedule()
{
    uint requested = fieldSchedule.settings.x;
    uint groups    = 0u;
    if (fieldControl[ZEN_FIELD_CONTROL_DONE] == 0u && fieldControl[ZEN_FIELD_CONTROL_PHASE] == requested)
    {
        uint first = 0u;
        uint count = 0u;
        if (requested == ZEN_FIELD_PHASE_CLEAR_BATCH)
        {
            groups = (FieldCellCount() + ZEN_FIELD_SCAN_GROUP - 1u) / ZEN_FIELD_SCAN_GROUP;
        }
        else if (requested == ZEN_FIELD_PHASE_SCAN || requested == ZEN_FIELD_PHASE_ALLOCATE)
        {
            groups = (FieldCellCount() + ZEN_FIELD_SCAN_GROUP - 1u) / ZEN_FIELD_SCAN_GROUP;
        }
        else if (requested == ZEN_FIELD_PHASE_JOBS)
        {
            groups = (FieldTriangleCount() + 63u) / 64u;
        }
        else if (requested == ZEN_FIELD_PHASE_COVERAGE)
        {
            first = fieldControl[ZEN_FIELD_CONTROL_COVERAGE_CURSOR];
            if (first == 0u)
            {
                fieldCounters[ZEN_FIELD_COUNTER_COVERAGE_PENDING] = 0u;
            }
            uint total = fieldCounters[ZEN_FIELD_COUNTER_COVERAGE_COUNT];
            // A few difficult alpha triangles can outlive thousands of completed ones.
            // Skip a bounded prefix here rather than submitting empty coverage dispatches
            // for every 64 finished entries on each refinement sweep.
            uint skipped = 0u;
            while (first < total && skipped < 1024u && fieldCoverage[first * ZEN_FIELD_COVERAGE_STATE_WORDS] != 1u)
            {
                ++first;
                ++skipped;
            }
            count  = min(total - min(first, total), fieldSchedule.settings.y);
            groups = (count + 63u) / 64u;
        }
        else if (requested == ZEN_FIELD_PHASE_COUNT || requested == ZEN_FIELD_PHASE_COMPACT)
        {
            uint cursor = requested == ZEN_FIELD_PHASE_COUNT ? ZEN_FIELD_CONTROL_JOB_CURSOR : ZEN_FIELD_CONTROL_COMPACT_CURSOR;
            first       = fieldControl[cursor];
            uint total  = min(fieldCounters[ZEN_FIELD_COUNTER_JOBS], fieldBuild.triangleRange.w);
            count       = min(total - min(first, total), fieldSchedule.settings.y);
            groups      = count;
        }
        else if (requested == ZEN_FIELD_PHASE_SCAN_SUMS)
        {
            groups = 1u;
        }
        else if (requested == ZEN_FIELD_PHASE_EMIT)
        {
            first      = fieldControl[ZEN_FIELD_CONTROL_ACTIVE_CURSOR];
            uint total = fieldCounters[ZEN_FIELD_COUNTER_ACTIVE];
            count      = min(total - min(first, total), fieldSchedule.settings.z);
            groups     = count;
        }
        else if (requested == ZEN_FIELD_PHASE_HIERARCHY)
        {
            uint side = FieldHierarchySide();
            groups    = (side * side * side + ZEN_FIELD_SCAN_GROUP - 1u) / ZEN_FIELD_SCAN_GROUP;
        }
        fieldControl[ZEN_FIELD_CONTROL_WORK_FIRST] = first;
        fieldControl[ZEN_FIELD_CONTROL_WORK_COUNT] = count;
    }
    // Empty indirect grids interleaved with barriers on the graphics queue faulted the NVIDIA
    // scheduler (Xid 13, SKEDCHECK36_DEPENDENCE_COUNTER_UNDERFLOW, device lost) during initial
    // builds. Most slices are idle, since only the current phase has work: launch one group
    // that exits at once instead.
    fieldControl[ZEN_FIELD_CONTROL_WORK_IDLE] = groups == 0u ? 1u : 0u;
    fieldIndirect[0]                          = max(groups, 1u);
    fieldIndirect[1]                          = 1u;
    fieldIndirect[2]                          = 1u;
    fieldIndirect[3]                          = 0u;
}
#endif

#ifdef FIELD_ADVANCE_SCHEDULE
void FieldAdvanceSchedule(uint requested)
{
    if (fieldControl[ZEN_FIELD_CONTROL_DONE] == 0u && fieldControl[ZEN_FIELD_CONTROL_PHASE] == requested)
    {
        uint failure = fieldCounters[ZEN_FIELD_COUNTER_FAILURE];
        uint retry   = fieldControl[ZEN_FIELD_CONTROL_BATCH_FAILURE];
        if (failure != 0u)
        {
            fieldControl[ZEN_FIELD_CONTROL_PHASE] = ZEN_FIELD_PHASE_FAILED;
            fieldControl[ZEN_FIELD_CONTROL_DONE]  = 2u;
        }
        else if (retry != 0u)
        {
            // Jobs, cell-reference counts and total references are validated
            // before allocation. Retrying never rolls back published atlas data.
            uint count = FieldTriangleCount();
            if (requested <= ZEN_FIELD_PHASE_SCAN_SUMS && count > 1u && fieldControl[ZEN_FIELD_CONTROL_COVERAGE_STREAMED] == 0u)
            {
                fieldControl[ZEN_FIELD_CONTROL_TRIANGLE_QUOTA]    = max(count / 2u, 1u);
                fieldControl[ZEN_FIELD_CONTROL_BATCH_FAILURE]     = 0u;
                fieldControl[ZEN_FIELD_CONTROL_SUCCESS_STREAK]    = 0u;
                fieldControl[ZEN_FIELD_CONTROL_COVERAGE_RESUME]   = 0u;
                fieldControl[ZEN_FIELD_CONTROL_COVERAGE_STREAMED] = 0u;
                fieldControl[ZEN_FIELD_CONTROL_PHASE]             = ZEN_FIELD_PHASE_CLEAR_BATCH;
                ++fieldControl[ZEN_FIELD_CONTROL_RETRIES];
            }
            else
            {
                fieldCounters[ZEN_FIELD_COUNTER_FAILURE] |= retry;
                fieldControl[ZEN_FIELD_CONTROL_PHASE]     = ZEN_FIELD_PHASE_FAILED;
                fieldControl[ZEN_FIELD_CONTROL_DONE]      = 2u;
            }
        }
        else if (requested == ZEN_FIELD_PHASE_CLEAR_BATCH)
        {
            fieldControl[ZEN_FIELD_CONTROL_JOB_CURSOR]      = 0u;
            fieldControl[ZEN_FIELD_CONTROL_COMPACT_CURSOR]  = 0u;
            fieldControl[ZEN_FIELD_CONTROL_ACTIVE_CURSOR]   = 0u;
            fieldControl[ZEN_FIELD_CONTROL_COVERAGE_CURSOR] = 0u;
            if (fieldControl[ZEN_FIELD_CONTROL_COVERAGE_RESUME] != 0u)
            {
                fieldControl[ZEN_FIELD_CONTROL_PHASE] = ZEN_FIELD_PHASE_COVERAGE;
            }
            else
            {
                uint remaining = fieldBuild.triangleRange.z - fieldControl[ZEN_FIELD_CONTROL_TRIANGLE_FIRST];
                fieldControl[ZEN_FIELD_CONTROL_TRIANGLE_COUNT] =
                    min(min(fieldControl[ZEN_FIELD_CONTROL_TRIANGLE_QUOTA], fieldBuild.triangleRange.y), remaining);
                fieldControl[ZEN_FIELD_CONTROL_PHASE] = remaining == 0u ? ZEN_FIELD_PHASE_HIERARCHY : ZEN_FIELD_PHASE_JOBS;
            }
        }
        else if (requested == ZEN_FIELD_PHASE_JOBS)
        {
            bool partial                                    = fieldCounters[ZEN_FIELD_COUNTER_COVERAGE_COUNT] > 0u;
            bool opaque                                     = fieldCounters[ZEN_FIELD_COUNTER_JOBS] > 0u;
            fieldControl[ZEN_FIELD_CONTROL_COVERAGE_RESUME] = partial && opaque ? 1u : 0u;
            fieldControl[ZEN_FIELD_CONTROL_PHASE] = partial && !opaque ? ZEN_FIELD_PHASE_COVERAGE : ZEN_FIELD_PHASE_COUNT;
        }
        else if (requested == ZEN_FIELD_PHASE_COVERAGE)
        {
            uint limit                                      = FieldCoverageJobLimit();
            fieldControl[ZEN_FIELD_CONTROL_COVERAGE_CURSOR] = FieldWorkFirst() + FieldWorkCount();
            if (fieldCounters[ZEN_FIELD_COUNTER_JOBS] >= limit)
            {
                // Reservations beyond this prefix retain their patch/region
                // cursor in coverage storage for the next stream.
                fieldCounters[ZEN_FIELD_COUNTER_JOBS]           = limit;
                fieldControl[ZEN_FIELD_CONTROL_COVERAGE_RESUME] = 1u;
                fieldControl[ZEN_FIELD_CONTROL_PHASE]           = ZEN_FIELD_PHASE_COUNT;
            }
            else if (fieldControl[ZEN_FIELD_CONTROL_COVERAGE_CURSOR] >= fieldCounters[ZEN_FIELD_COUNTER_COVERAGE_COUNT])
            {
                fieldControl[ZEN_FIELD_CONTROL_COVERAGE_CURSOR] = 0u;
                if (fieldCounters[ZEN_FIELD_COUNTER_COVERAGE_PENDING] == 0u)
                {
                    fieldControl[ZEN_FIELD_CONTROL_COVERAGE_RESUME] = 0u;
                    fieldControl[ZEN_FIELD_CONTROL_PHASE]           = ZEN_FIELD_PHASE_COUNT;
                }
            }
        }
        else if (requested == ZEN_FIELD_PHASE_COUNT)
        {
            fieldControl[ZEN_FIELD_CONTROL_JOB_CURSOR] += FieldWorkCount();
            if (fieldControl[ZEN_FIELD_CONTROL_JOB_CURSOR] >= fieldCounters[ZEN_FIELD_COUNTER_JOBS])
            {
                fieldControl[ZEN_FIELD_CONTROL_PHASE] = ZEN_FIELD_PHASE_SCAN;
            }
        }
        else if (requested == ZEN_FIELD_PHASE_SCAN)
        {
            fieldControl[ZEN_FIELD_CONTROL_PHASE] = ZEN_FIELD_PHASE_SCAN_SUMS;
        }
        else if (requested == ZEN_FIELD_PHASE_SCAN_SUMS)
        {
            fieldControl[ZEN_FIELD_CONTROL_PHASE] = ZEN_FIELD_PHASE_ALLOCATE;
        }
        else if (requested == ZEN_FIELD_PHASE_ALLOCATE)
        {
            fieldControl[ZEN_FIELD_CONTROL_PHASE] = ZEN_FIELD_PHASE_COMPACT;
        }
        else if (requested == ZEN_FIELD_PHASE_COMPACT)
        {
            fieldControl[ZEN_FIELD_CONTROL_COMPACT_CURSOR] += FieldWorkCount();
            if (fieldControl[ZEN_FIELD_CONTROL_COMPACT_CURSOR] >= fieldCounters[ZEN_FIELD_COUNTER_JOBS])
            {
                fieldControl[ZEN_FIELD_CONTROL_PHASE] = ZEN_FIELD_PHASE_EMIT;
            }
        }
        else if (requested == ZEN_FIELD_PHASE_EMIT)
        {
            fieldControl[ZEN_FIELD_CONTROL_ACTIVE_CURSOR] += FieldWorkCount();
            if (fieldControl[ZEN_FIELD_CONTROL_ACTIVE_CURSOR] >= fieldCounters[ZEN_FIELD_COUNTER_ACTIVE])
            {
                if (fieldControl[ZEN_FIELD_CONTROL_COVERAGE_RESUME] != 0u)
                {
                    fieldControl[ZEN_FIELD_CONTROL_COVERAGE_STREAMED] = 1u;
                    fieldControl[ZEN_FIELD_CONTROL_PHASE]             = ZEN_FIELD_PHASE_CLEAR_BATCH;
                }
                else
                {
                    fieldControl[ZEN_FIELD_CONTROL_COVERAGE_STREAMED]    = 0u;
                    fieldControl[ZEN_FIELD_CONTROL_TRIANGLE_FIRST]      += FieldTriangleCount();
                    fieldControl[ZEN_FIELD_CONTROL_PROCESSED_TRIANGLES]  = fieldControl[ZEN_FIELD_CONTROL_TRIANGLE_FIRST];
                    ++fieldControl[ZEN_FIELD_CONTROL_COMPLETED_BATCHES];
                    // Recover after a dense region without immediately retrying the
                    // same oversized quota after every accepted batch. Growth is bounded
                    // by the scene (the owner's batch cap applies separately), and
                    // requires two accepted batches that used at most half of every
                    // capacity a doubled batch of similar density could overflow. Growing
                    // after any two successes revisited oversized batches in dense
                    // regions: 1,687 retries on New Sponza (BrixelizerGIProfiling.md).
                    bool headroom = fieldCounters[ZEN_FIELD_COUNTER_JOBS] * 2u <= fieldBuild.triangleRange.w
                                 && fieldCounters[ZEN_FIELD_COUNTER_REFERENCES] * 2u <= fieldBuild.limits.x
                                 && fieldCounters[ZEN_FIELD_COUNTER_CELL_PEAK] * 2u <= fieldBuild.limits.y;
                    fieldControl[ZEN_FIELD_CONTROL_SUCCESS_STREAK] =
                        headroom ? fieldControl[ZEN_FIELD_CONTROL_SUCCESS_STREAK] : 0u;
                    ++fieldControl[ZEN_FIELD_CONTROL_SUCCESS_STREAK];
                    if (fieldControl[ZEN_FIELD_CONTROL_SUCCESS_STREAK] == 2u)
                    {
                        fieldControl[ZEN_FIELD_CONTROL_TRIANGLE_QUOTA] =
                            min(fieldControl[ZEN_FIELD_CONTROL_TRIANGLE_QUOTA] * 2u, max(fieldBuild.triangleRange.z, 1u));
                        fieldControl[ZEN_FIELD_CONTROL_SUCCESS_STREAK] = 0u;
                    }
                    fieldControl[ZEN_FIELD_CONTROL_PHASE] =
                        fieldControl[ZEN_FIELD_CONTROL_TRIANGLE_FIRST] == fieldBuild.triangleRange.z
                            ? ZEN_FIELD_PHASE_HIERARCHY
                            : ZEN_FIELD_PHASE_CLEAR_BATCH;
                }
            }
        }
        else if (requested == ZEN_FIELD_PHASE_HIERARCHY)
        {
            uint side = FieldHierarchySide();
            if (side == 1u)
            {
                fieldControl[ZEN_FIELD_CONTROL_PHASE] = ZEN_FIELD_PHASE_COMPLETE;
                fieldControl[ZEN_FIELD_CONTROL_DONE]  = 1u;
            }
            else
            {
                fieldControl[ZEN_FIELD_CONTROL_CHILD_OFFSET]   = fieldControl[ZEN_FIELD_CONTROL_PARENT_OFFSET];
                fieldControl[ZEN_FIELD_CONTROL_PARENT_OFFSET] += side * side * side;
                ++fieldControl[ZEN_FIELD_CONTROL_HIERARCHY_LEVEL];
            }
        }
    }
}
#endif
#endif
