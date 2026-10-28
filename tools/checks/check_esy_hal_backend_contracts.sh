#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DRIVER_DIR="${ROOT_DIR}/esy_hal_backend"

require_tool() {
  local tool="$1"

  if ! command -v "${tool}" >/dev/null 2>&1; then
    echo "error: ${tool} is required for esy_hal_backend platform contract checks" >&2
    exit 127
  fi
}

module_header_for_source() {
  local source_relative="$1"
  local header="${DRIVER_DIR}/include/esy_hal_backend/${source_relative%.c}.h"

  if [[ -f "${header}" ]]; then
    printf '%s\n' "${header}"
  fi
}

extract_declared_functions() {
  perl -0777 -ne '
    while (/\b(esy_hb_[A-Za-z0-9_]+)\s*\([^;{}]*\)\s*;/g) {
      print "$1\n";
    }
  ' "$@" | sort -u
}

extract_defined_functions() {
  perl -0777 -ne '
    while (/(?:^|\n)\s*(?:extern\s+)?(?:[A-Za-z_][A-Za-z0-9_]*\s+|const\s+|volatile\s+|\*)+\s*(esy_hb_[A-Za-z0-9_]+)\s*\([^;{}]*\)\s*\{/g) {
      print "$1\n";
    }
  ' "$@" | sort -u
}

require_tool perl
require_tool comm
require_tool python3

status=0

echo "[esy_hal_backend] checking platform source/header API closure and Flash geometry contract"
for platform_dir in "${DRIVER_DIR}"/src/*; do
  [[ -d "${platform_dir}" ]] || continue
  platform="$(basename "${platform_dir}")"

  while IFS= read -r -d '' source; do
    source_relative="${source#"${platform_dir}/"}"
    header="$(module_header_for_source "${source_relative}")"
    [[ -n "${header}" ]] || continue

    declared_file="$(mktemp)"
    defined_file="$(mktemp)"
    trap 'rm -f "${declared_file}" "${defined_file}"' RETURN

    extract_declared_functions "${header}" >"${declared_file}"
    if [[ "${source_relative}" == "esy_hb_boot.c" && "${platform}" != "esp32" ]]; then
      extract_defined_functions "${source}" "${DRIVER_DIR}/src/esy_hb_boot_direct.c" >"${defined_file}"
    else
      extract_defined_functions "${source}" >"${defined_file}"
    fi

    missing="$(comm -23 "${declared_file}" "${defined_file}")"
    rm -f "${declared_file}" "${defined_file}"
    trap - RETURN

    if [[ -n "${missing}" ]]; then
      echo "error: ${platform}/${source_relative} misses public API definitions from ${header#${ROOT_DIR}/}:" >&2
      printf '%s\n' "${missing}" | sed 's/^/  - /' >&2
      status=1
    fi
  done < <(find "${platform_dir}" -name 'esy_hb_*.c' -type f -print0 | sort -z)
done

echo "[esy_hal_backend] checking placeholder success stubs"
while IFS= read -r -d '' source; do
  matches="$(
    perl -0777 -ne '
      while (/\besy_err_t\s+(esy_hb_[A-Za-z0-9_]+)\s*\([^)]*\)\s*\{(.*?)\n\}/sg) {
        my ($name, $body) = ($1, $2);
        $body =~ s{/[*].*?[*]/}{}sg;
        $body =~ s{//[^\n]*}{}g;
        $body =~ s/\s+/ /g;
        $body =~ s/^\s+|\s+$//g;

        my $without_void = $body;
        $without_void =~ s/\(void\)\s*[A-Za-z_][A-Za-z0-9_]*\s*;\s*//g;
        $without_void =~ s/^\s+|\s+$//g;

        if ($without_void eq "return ESY_OK;") {
          print "$name\n";
        }
      }
    ' "${source}"
  )"

  if [[ -n "${matches}" ]]; then
    echo "error: status-returning placeholder stub must not return ESY_OK: ${source#${ROOT_DIR}/}" >&2
    printf '%s\n' "${matches}" | sed 's/^/  - /' >&2
    status=1
  fi
done < <(find "${DRIVER_DIR}/src" -name 'esy_hb_*.c' -type f -print0 | sort -z)

echo "[esy_hal_backend] checking STM32F4 always-on timebase honesty"
stm32_system="${DRIVER_DIR}/src/stm32f4/esy_hb_system.c"
if grep -Eq 'TIM6|__HAL_TIM_GET_COUNTER' "${stm32_system}"; then
  echo "error: STM32F4 system time must not present TIM6 as a monotonic or always-on source" >&2
  status=1
fi
if ! grep -q 'esy_hb_system_always_on_time_us' "${stm32_system}" ||
   ! grep -q 'ESY_ERR_NOT_SUPPORTED' "${stm32_system}"; then
  echo "error: STM32F4 must fail closed when no compliant always-on counter exists" >&2
  status=1
fi

echo "[esy_hal_backend] checking STM32F4 bounded STOP clock restore"
stm32_low_power="${DRIVER_DIR}/src/stm32f4/esy_hb_low_power.c"
if grep -Eq 'HAL_RCC_(OscConfig|ClockConfig)|HAL_GetTick' "${stm32_low_power}"; then
  echo "error: STM32F4 STOP restore must not use HAL tick-dependent RCC waits" >&2
  status=1
fi
if ! grep -q 'DWT->CYCCNT' "${stm32_low_power}" ||
   ! grep -q 'remaining_iterations' "${stm32_low_power}" ||
   ! grep -q 'ESY_ERR_TIMEOUT' "${stm32_low_power}"; then
  echo "error: STM32F4 STOP restore must retain cycle and iteration timeout bounds" >&2
  status=1
fi

echo "[esy_hal_backend] checking address, deadline and native preflight contracts"
if ! python3 - "${DRIVER_DIR}" <<'PY'
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1])
errors = []

def require(condition, message):
    if not condition:
        errors.append(message)

def body(source, name):
    match = re.search(r'\b' + name + r'\s*\([^;{}]*\)\s*\{', source)
    if not match:
        errors.append(f'missing function body: {name}')
        return ''
    begin = match.end()
    depth = 1
    for end in range(begin, len(source)):
        depth += (source[end] == '{') - (source[end] == '}')
        if depth == 0:
            return source[begin:end]
    errors.append(f'unclosed function body: {name}')
    return ''

for port in ('pico', 'stm32f4', 'esp32'):
    system_source = (root / f'src/{port}/esy_hb_system.c').read_text()
    boot_source = (root / f'src/{port}/esy_hb_boot.c').read_text()
    require('jump_to_app' not in system_source and
            'esy_hb_boot_private.h' not in system_source and
            'esp_ota_' not in system_source,
            f'{port}: system must not own image handoff or native boot policy')
    handoff = body(boot_source, 'esy_hb_boot_jump_to_app')
    if port == 'pico':
        require('msr msp' in handoff and 'bx r1' in handoff,
                'Pico boot must preserve its vector/MSP/native branch sequence')
    elif port == 'stm32f4':
        require('__get_IPSR()' in handoff and '__get_CONTROL()' in handoff and
                'HAL_SuspendTick()' in handoff and 'NVIC->ICER' in handoff and
                'NVIC->ICPR' in handoff and 'SCB->VTOR' in handoff and
                'enter_application(initial_msp, reset_handler)' in handoff,
                'STM32F4 boot must preserve CPU guards and IRQ/vector preparation')
    else:
        require('native_boot_find_image' in handoff and
                'native_boot_has_two_ota_images' in handoff and
                handoff.count('esp_restart()') == 2 and
                handoff.index('next->address == app_address') <
                handoff.index('native_boot_set_pending_image'),
                'ESP32 boot must preserve native validation and selected-trial restart')

signature = (r'\besy_hb_i2c_(?:write|read)_(?:blocking|dma)\s*\('
             r'\s*esy_i2c_t\s*\*\s*i2c\s*,\s*uint16_t\s+addr\b')
for relative in ['include/esy_hal_backend/esy_hb_i2c.h'] + [
        f'src/{port}/esy_hb_i2c.c' for port in ('pico', 'stm32f4', 'esp32')]:
    source = (root / relative).read_text()
    require(len(re.findall(signature, source)) == 4,
            f'{relative}: all I2C target arguments must preserve uint16_t')
    require('(void)xfer->timeout_ms' not in source,
            f'{relative}: transaction timeouts must not be discarded')

pico = (root / 'src/pico/esy_hb_i2c.c').read_text()
transfer = body(pico, 'esy_hb_i2c_transfer')
require('i2c_write_blocking_until' in transfer and 'i2c_read_blocking_until' in transfer
        and transfer.count('make_timeout_time_ms') == 1,
        'Pico I2C must share one absolute deadline across native phases')
require(not re.search(r'\bi2c_(?:read|write)_blocking\s*\(', pico),
        'Pico I2C must not use unbounded native blocking transfers')
require('xfer->addr > ESY_I2C_ADDR_7BIT_MAX' in transfer and
        transfer.index('xfer->addr > ESY_I2C_ADDR_7BIT_MAX') < transfer.index('(uint8_t)xfer->addr'),
        'Pico I2C must validate its full address before narrowing for the SDK')

stm32 = (root / 'src/stm32f4/esy_hb_i2c.c').read_text()
transfer = body(stm32, 'esy_hb_i2c_transfer')
require('HAL_MAX_DELAY' not in stm32 and 'budget_ms - elapsed_ms' in transfer
        and 'HAL_GetTick() - started_ms' in transfer,
        'STM32F4 I2C must pass a finite remaining transaction budget')

esp32 = (root / 'src/esp32/esy_hb_i2c.c').read_text()
transfer = body(esp32, 'i2c_transfer_until')
require('deadline_us - esp_timer_get_time()' in transfer and
        transfer.count('i2c_transfer_until(context, &phase, deadline_us)') == 2,
        'ESP32 separate I2C phases must retain the original deadline')

spi = body((root / 'src/pico/esy_hb_spi.c').read_text(), 'esy_hb_spi_init')
require('clock_get_hz(clk_peri)' in spi and
        not re.search(r'spi_init\([^;]*:\s*0u?\s*\)', spi),
        'Pico SPI slave SDK initialization requires a valid nonzero divider')

gpio = body((root / 'src/esp32/esy_hb_gpio.c').read_text(), 'esy_hb_gpio_init')
require('s_ctx.handlers[pin] != NULL' in gpio and
        gpio.index('return ESY_ERR_BUSY') < gpio.index('gpio_config(&io_conf)'),
        'ESP32 electrical-state changes must reject attached IRQs before native mutation')
adc_source = (root / 'src/stm32f4/esy_hb_adc.c').read_text()
adc_available = body(adc_source, 'esy_hb_adc_check_available')
require('s_ctx.slots[phys_index].initialized' in adc_available and
        'ESY_ERR_BUSY' in adc_available and 'decode_adc(phys_index)' in adc_available,
        'ADC physical ownership preflight must reject unavailable or occupied controllers')
adc_init = body(adc_source, 'esy_hb_adc_init')
require('esy_hb_adc_check_available(config->phys_index)' in adc_init and
        adc_init.index('if (available != ESY_OK) return available') < adc_init.index('memset(slot'),
        'ADC native init must recheck availability before changing its physical slot')

xio_spi = (root / 'src/pico/xio/spi/esy_hb_xio_spi.c').read_text()
xio_pio = (root / 'src/pico/xio/spi/esy_hb_xio_spi.pio').read_text()
monitor = body(xio_spi, 'cs_monitor_init')
require('esy_hb_resource_assembly_try_acquire()' in monitor and
        'config->slave_cs_sm_index' in monitor and 'pio_sm_claim(pio, smi)' in monitor and
        'pio_add_program(pio, &xio_spi_slave_cs_program)' in monitor and
        'sm_config_set_clkdiv_int_frac(&sc, 1u, 0u)' in monitor and
        'sm_config_set_sideset(&sc, 1u, false, true)' in monitor,
        'Pico XIO slave MISO requires an independently claimed full-speed OE monitor')
require(re.search(r'\.program xio_spi_slave_cs\s+\.side_set 1 pindirs\s+'
                  r'wait 0 pin 0 side 0[^\n]*\s+wait 1 pin 0 side 1', xio_pio) is not None,
        'Pico XIO CS monitor must release MISO while waiting for selection')
slave = body(xio_spi, 'xio_spi_load_and_config_slave')
require('sc.out_pin_base = ESY_PIN_UNUSED' in slave and
        'sc.set_pin_base = ESY_PIN_UNUSED' in slave and
        not re.search(r'pio_sm_set_consecutive_pindirs\([^;]*pin_miso[^;]*true', slave) and
        'hw_clear_bits(&pio->input_sync_bypass, 1u << xcfg->pin_cs)' in slave,
        'Pico XIO data SM must not enable MISO or bypass CS synchronization')
require('cs_monitor_release(c)' in body(xio_spi, 'esy_hb_xio_spi_deinit'),
        'Pico XIO SPI teardown must release its CS monitor')
for name in ('esy_hb_xio_spi_transfer_blocking', 'esy_hb_xio_spi_transfer16_blocking'):
    transfer = body(xio_spi, name)
    require('wait_tx_done_blocking_slave' not in transfer and
            'const uint32_t wrx = pio->rxf[smi]' in transfer and
            'if (rx_buf) rx_buf[i]' in transfer,
            f'{name}: slave TX-only must retire received frames without waiting for another CS')
poll = body(xio_spi, 'esy_hb_xio_spi_poll')
require('if (is_master(c) && c->pin_trigger != ESY_PIN_UNUSED)' in poll and
        'else if (is_master(c))' in poll,
        'Pico XIO slave DMA retirement must not wait for a future CS/TXSTALL')

dma = body((root / 'src/stm32f4/esy_hb_dma.c').read_text(), 'esy_hb_dma_prepare')
guard = re.search(r'ESY_DMA_MEM_TO_MEM\s*&&\s*map->controller\s*!=\s*DMA2\)\s*'
                  r'return ESY_ERR_NOT_SUPPORTED;', dma)
require(guard is not None and guard.start() < dma.index('esy_hb_dma_claim('),
        'STM32F4 DMA1 memory copies must reject before resource claiming')

watchdog = (root / 'src/stm32f4/esy_hb_watchdog.c').read_text()
require('defined(DBGMCU_APB1_FZ_DBG_IWDG_STOP)' in watchdog and
        '__HAL_DBGMCU_FREEZE_IWDG();' in watchdog and
        '__HAL_DBGMCU_UNFREEZE_IWDG();' in watchdog and
        'defined(DBGMCU_APB1_FZ_IWDG_STOP)' not in watchdog,
        'STM32F4 watchdog must apply both supported debug-freeze policies')

for error in errors:
    print(f'error: {error}', file=sys.stderr)
sys.exit(bool(errors))
PY
then
  status=1
fi

if ((status != 0)); then
  echo "[esy_hal_backend] platform contract checks failed" >&2
  exit "${status}"
fi

echo "[esy_hal_backend] platform contract checks passed"
