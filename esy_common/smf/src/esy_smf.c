/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_common/smf/esy_smf.h"

static esy_err_t validate_config(const esy_smf_config_t *config) {
  if ((config->states == NULL) || (config->state_count == 0u) ||
      (config->state_count == ESY_SMF_STATE_NONE)) {
    return ESY_ERR_INVALID_ARG;
  }
  /* Validate all indices before traversing any parent or initial chain. */
  for (uint32_t i = 0u; i < config->state_count; ++i) {
    const esy_smf_state_t *state = &config->states[i];
    if (((state->parent != ESY_SMF_STATE_NONE) &&
         (state->parent >= config->state_count)) ||
        ((state->initial != ESY_SMF_STATE_NONE) &&
         (state->initial >= config->state_count))) {
      return ESY_ERR_INVALID_ARG;
    }
  }
  for (uint32_t i = 0u; i < config->state_count; ++i) {
    const esy_smf_state_t *state = &config->states[i];
    if ((state->initial != ESY_SMF_STATE_NONE) &&
        (config->states[state->initial].parent != i)) {
      return ESY_ERR_INVALID_ARG;
    }
    esy_smf_state_id_t cursor = i;
    uint32_t visited = 0u;
    while (cursor != ESY_SMF_STATE_NONE) {
      if (visited == config->state_count) return ESY_ERR_INVALID_ARG;
      ++visited;
      cursor = config->states[cursor].parent;
    }
  }
  return ESY_OK;
}

static esy_smf_state_id_t parent_of(const esy_smf_t *machine,
                                    esy_smf_state_id_t state) {
  return state == ESY_SMF_STATE_NONE ? ESY_SMF_STATE_NONE
                                    : machine->config.states[state].parent;
}

static uint32_t state_depth(const esy_smf_t *machine,
                            esy_smf_state_id_t state) {
  uint32_t depth = 0u;
  while (state != ESY_SMF_STATE_NONE) {
    ++depth;
    state = parent_of(machine, state);
  }
  return depth;
}

static esy_smf_state_id_t common_ancestor(const esy_smf_t *machine,
                                          esy_smf_state_id_t first,
                                          esy_smf_state_id_t second) {
  uint32_t first_depth = state_depth(machine, first);
  uint32_t second_depth = state_depth(machine, second);
  while (first_depth > second_depth) {
    first = parent_of(machine, first);
    --first_depth;
  }
  while (second_depth > first_depth) {
    second = parent_of(machine, second);
    --second_depth;
  }
  while (first != second) {
    first = parent_of(machine, first);
    second = parent_of(machine, second);
  }
  return first;
}

static esy_smf_state_id_t initial_leaf(const esy_smf_t *machine,
                                      esy_smf_state_id_t target) {
  while (machine->config.states[target].initial != ESY_SMF_STATE_NONE) {
    target = machine->config.states[target].initial;
  }
  return target;
}

static esy_err_t exit_to(esy_smf_t *machine, esy_smf_state_id_t boundary) {
  while (machine->current != boundary) {
    const esy_smf_state_t *state = &machine->config.states[machine->current];
    if (state->exit != NULL) {
      esy_err_t ret = state->exit(machine->config.user);
      if (ret != ESY_OK) {
        machine->status = ESY_SMF_FAULTED;
        return ret;
      }
    }
    machine->current = state->parent;
  }
  return ESY_OK;
}

static esy_err_t enter_to(esy_smf_t *machine, esy_smf_state_id_t target) {
  while (machine->current != target) {
    /* Walk back from the target instead of allocating an ancestor stack. */
    esy_smf_state_id_t next = target;
    while (parent_of(machine, next) != machine->current) {
      next = parent_of(machine, next);
    }
    const esy_smf_state_t *state = &machine->config.states[next];
    if (state->entry != NULL) {
      esy_err_t ret = state->entry(machine->config.user);
      if (ret != ESY_OK) {
        machine->status = ESY_SMF_FAULTED;
        return ret;
      }
    }
    machine->current = next;
  }
  return ESY_OK;
}

static esy_err_t transition_from(esy_smf_t *machine,
                                  esy_smf_state_id_t source,
                                  esy_smf_state_id_t target) {
  esy_smf_state_id_t boundary = common_ancestor(machine, source, target);
  /* External transitions reenter their source/target ancestor, including self.
   * Compute this boundary before following the target's initial children. */
  if ((boundary == source) || (boundary == target)) {
    boundary = parent_of(machine, boundary);
  }
  esy_err_t ret = exit_to(machine, boundary);
  if (ret != ESY_OK) return ret;
  return enter_to(machine, initial_leaf(machine, target));
}

esy_err_t esy_smf_init(esy_smf_t *machine, const esy_smf_config_t *config) {
  if ((machine == NULL) || (config == NULL)) return ESY_ERR_INVALID_ARG;
  if (machine->busy) return ESY_ERR_BUSY;
  if (machine->status != ESY_SMF_UNINITIALIZED) return ESY_ERR_STATE;
  esy_err_t ret = validate_config(config);
  if (ret != ESY_OK) return ret;
  *machine = (esy_smf_t){
      .config = *config,
      .current = ESY_SMF_STATE_NONE,
      .status = ESY_SMF_READY,
      .busy = false,
  };
  return ESY_OK;
}

esy_err_t esy_smf_start(esy_smf_t *machine, esy_smf_state_id_t initial) {
  if (machine == NULL) return ESY_ERR_INVALID_ARG;
  if (machine->busy) return ESY_ERR_BUSY;
  if (machine->status != ESY_SMF_READY) return ESY_ERR_STATE;
  if (initial >= machine->config.state_count) return ESY_ERR_INVALID_ARG;
  machine->busy = true;
  machine->status = ESY_SMF_RUNNING;
  esy_err_t ret = enter_to(machine, initial_leaf(machine, initial));
  machine->busy = false;
  return ret;
}

esy_err_t esy_smf_run(esy_smf_t *machine, const void *event) {
  if (machine == NULL) return ESY_ERR_INVALID_ARG;
  if (machine->busy) return ESY_ERR_BUSY;
  if (machine->status != ESY_SMF_RUNNING) return ESY_ERR_STATE;
  machine->busy = true;
  esy_err_t ret = ESY_OK;
  for (esy_smf_state_id_t state = machine->current;
       state != ESY_SMF_STATE_NONE; state = parent_of(machine, state)) {
    esy_smf_run_fn_t run = machine->config.states[state].run;
    if (run == NULL) continue;
    esy_smf_decision_t decision = {ESY_SMF_HANDLED, ESY_SMF_STATE_NONE};
    ret = run(machine->config.user, event, &decision);
    if (ret != ESY_OK) {
      machine->status = ESY_SMF_FAULTED;
      break;
    }
    if (decision.action == ESY_SMF_PROPAGATE) continue;
    if (decision.action == ESY_SMF_HANDLED) break;
    if ((decision.action != ESY_SMF_TRANSITION) ||
        (decision.target >= machine->config.state_count)) {
      machine->status = ESY_SMF_FAULTED;
      ret = ESY_ERR_INVALID_ARG;
      break;
    }
    ret = transition_from(machine, state, decision.target);
    break;
  }
  machine->busy = false;
  return ret;
}

esy_err_t esy_smf_transition(esy_smf_t *machine, esy_smf_state_id_t target) {
  if (machine == NULL) return ESY_ERR_INVALID_ARG;
  if (machine->busy) return ESY_ERR_BUSY;
  if (machine->status != ESY_SMF_RUNNING) return ESY_ERR_STATE;
  if (target >= machine->config.state_count) return ESY_ERR_INVALID_ARG;
  machine->busy = true;
  esy_err_t ret = transition_from(machine, machine->current, target);
  machine->busy = false;
  return ret;
}

esy_err_t esy_smf_stop(esy_smf_t *machine) {
  if (machine == NULL) return ESY_ERR_INVALID_ARG;
  if (machine->busy) return ESY_ERR_BUSY;
  if (machine->status == ESY_SMF_UNINITIALIZED) return ESY_ERR_STATE;
  machine->busy = true;
  esy_err_t ret = exit_to(machine, ESY_SMF_STATE_NONE);
  if (ret == ESY_OK) machine->status = ESY_SMF_READY;
  machine->busy = false;
  return ret;
}

esy_err_t esy_smf_deinit(esy_smf_t *machine) {
  if (machine == NULL) return ESY_ERR_INVALID_ARG;
  if (machine->busy) return ESY_ERR_BUSY;
  if (machine->status != ESY_SMF_READY) return ESY_ERR_STATE;
  *machine = (esy_smf_t){0};
  return ESY_OK;
}

esy_err_t esy_smf_get_state(const esy_smf_t *machine, esy_smf_state_id_t *state) {
  if ((machine == NULL) || (state == NULL)) return ESY_ERR_INVALID_ARG;
  if (machine->status == ESY_SMF_UNINITIALIZED) return ESY_ERR_STATE;
  *state = machine->current;
  return ESY_OK;
}

esy_err_t esy_smf_get_status(const esy_smf_t *machine, esy_smf_status_t *status) {
  if ((machine == NULL) || (status == NULL)) return ESY_ERR_INVALID_ARG;
  *status = machine->status;
  return ESY_OK;
}
