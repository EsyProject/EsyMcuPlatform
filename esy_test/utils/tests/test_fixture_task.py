# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Exercise task fixture allocation and recovery with the shared OS fakes."""

from pathlib import Path
import subprocess
import tempfile
import unittest


class FixtureTaskTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="esy-task-fixture-")
        directory = Path(cls.temporary.name)
        root = Path(__file__).resolve().parents[3]
        driver = directory / "driver.c"
        driver.write_text(r'''#include "esy_fixture_task.h"
#include "fake_esy_os_task.h"
#include <assert.h>
#include <string.h>

static void task_entry(void *arg) { (void)arg; }

static esy_err_t create(esy_os_task_t **task) {
  return esy_fixture_ensure_task("test", task_entry, NULL, 256u, 1u, task);
}

static void fill_capacity(void) {
  esy_os_task_t *tasks[FAKE_ESY_OS_TASK_MAX_TASKS] = {0};
  for (unsigned i = 0u; i < FAKE_ESY_OS_TASK_MAX_TASKS; ++i) {
    assert(create(&tasks[i]) == ESY_OK);
    fake_esy_os_task_entry_t *current = fake_esy_os_task_find(tasks[i]);
    assert(current != NULL && current->used);
    for (unsigned j = 0u; j < i; ++j) {
      fake_esy_os_task_entry_t *previous = fake_esy_os_task_find(tasks[j]);
      assert(tasks[i] != tasks[j]);
      assert(current->stack_buffer != previous->stack_buffer);
      assert(current->static_storage != previous->static_storage);
    }
  }
  uint32_t calls = fake_esy_os_task_get_create_static_count();
  esy_os_task_t *extra = NULL;
  assert(create(&extra) == ESY_ERR_INVALID_ARG);
  assert(extra == NULL);
  assert(fake_esy_os_task_get_create_static_count() == calls);
  assert(create(&tasks[0]) == ESY_OK);
  assert(fake_esy_os_task_get_create_static_count() == calls);
}

int main(int argc, char **argv) {
  assert(argc == 2);
  esy_fixture_reset_os_task_and_queue();
  if (strcmp(argv[1], "failure") == 0) {
    esy_os_task_t *task = NULL;
    fake_esy_os_task_set_create_result(ESY_ERR_NO_MEM);
    for (unsigned i = 0u; i <= FAKE_ESY_OS_TASK_MAX_TASKS; ++i) {
      assert(create(&task) == ESY_ERR_NO_MEM);
      assert(task == NULL);
    }
    fake_esy_os_task_set_create_result(ESY_OK);
    fill_capacity();
  } else if (strcmp(argv[1], "partial") == 0) {
    esy_os_task_t *first = NULL;
    esy_os_task_t *second = NULL;
    assert(create(&first) == ESY_OK);
    fake_esy_os_task_entry_t original = *fake_esy_os_task_find(first);
    fake_esy_os_task_set_create_result(ESY_ERR_NO_MEM);
    for (unsigned i = 0u; i < FAKE_ESY_OS_TASK_MAX_TASKS; ++i)
      assert(create(&second) == ESY_ERR_NO_MEM);
    fake_esy_os_task_set_create_result(ESY_OK);
    assert(create(&second) == ESY_OK);
    assert(memcmp(&original, fake_esy_os_task_find(first), sizeof(original)) == 0);
    assert(original.stack_buffer != fake_esy_os_task_find(second)->stack_buffer);
    assert(original.static_storage != fake_esy_os_task_find(second)->static_storage);
  } else if (strcmp(argv[1], "reset") == 0) {
    fill_capacity();
    esy_fixture_reset_os_task_and_queue();
    fill_capacity();
    assert(fake_esy_os_task_get_create_static_count() == FAKE_ESY_OS_TASK_MAX_TASKS);
  } else {
    esy_os_task_t *task = NULL;
    assert(esy_fixture_ensure_task(NULL, task_entry, NULL, 256u, 1u, &task) == ESY_ERR_INVALID_ARG);
    assert(esy_fixture_ensure_task("test", NULL, NULL, 256u, 1u, &task) == ESY_ERR_INVALID_ARG);
    assert(esy_fixture_ensure_task("test", task_entry, NULL, 256u, 1u, NULL) == ESY_ERR_INVALID_ARG);
    assert(esy_fixture_ensure_task("test", task_entry, NULL, 0u, 1u, &task) == ESY_ERR_INVALID_ARG);
    assert(esy_fixture_ensure_task("test", task_entry, NULL, 1025u, 1u, &task) == ESY_ERR_INVALID_ARG);
    assert(fake_esy_os_task_get_create_static_count() == 0u);
    fill_capacity();
  }
  return 0;
}
''')
        cls.executable = directory / "fixture"
        includes = [root / "esy_test/utils", root / "esy_test/fake/os",
                    root / "esy_middleware/os/include",
                    root / "esy_common/defs/include"]
        command = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror"]
        for include in includes:
            command.extend(["-I", str(include)])
        command.extend([str(driver), str(root / "esy_test/utils/esy_fixture_task.c"),
                        str(root / "esy_test/fake/os/fake_esy_os_task.c"),
                        str(root / "esy_test/fake/os/fake_esy_os_queue.c"),
                        "-o", str(cls.executable)])
        subprocess.run(command, check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def run_case(self, case):
        subprocess.run([str(self.executable), case], check=True,
                       capture_output=True, text=True)

    def test_failed_creation_preserves_all_backing_capacity(self):
        self.run_case("failure")

    def test_failure_after_success_preserves_the_existing_task(self):
        self.run_case("partial")

    def test_reset_reclaims_all_successful_slots(self):
        self.run_case("reset")

    def test_invalid_arguments_preserve_capacity(self):
        self.run_case("invalid")


if __name__ == "__main__":
    unittest.main()
