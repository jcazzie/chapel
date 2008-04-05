//
// This file can hold shared code between the different thread
// implementation files.
//
#include "chplrt.h"
#include "chplthreads.h"
#include "chplmem.h"

void chpl_add_to_task_list (chpl_threadfp_t fun, chpl_threadarg_t arg, chpl_task_list_p *task_list) {
  chpl_task_list_p task = (chpl_task_list_p)_chpl_malloc(1, sizeof(chpl_task_list_p), "task list entry", 0, 0);
  task->fun = fun;
  task->arg = arg;
  task->next = *task_list;
  *task_list = task;
}

void chpl_process_task_list (chpl_task_list_p task_list) {
  if (task_list) {
    chpl_process_task_list(task_list->next); // Add the last task on list first!
    chpl_begin (task_list->fun, task_list->arg, false, false);
    _chpl_free (task_list, 0, 0);
  }
}
