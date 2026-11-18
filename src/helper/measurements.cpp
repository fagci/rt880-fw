#include "measurements.h"
#include "math_util.h"
#include <stdint.h>

extern uint32_t _sheap;
extern uint32_t _eheap;
static uint32_t heap_ptr = 0;

extern "C" void *_sbrk(ptrdiff_t incr) {
  if (heap_ptr == 0) {
    heap_ptr = reinterpret_cast<uint32_t>(&_sheap);
  }
  if (incr > 0 && heap_ptr + incr > reinterpret_cast<uint32_t>(&_eheap)) {
    return reinterpret_cast<void *>(-1);
  }
  void *prev = reinterpret_cast<void *>(heap_ptr);
  heap_ptr += incr;
  return prev;
}

long long Clamp(long long v, long long min, long long max) {
  return v <= min ? min : (v >= max ? max : v);
}

int ConvertDomain(int aValue, int aMin, int aMax, int bMin, int bMax) {
  const int aRange = aMax - aMin;
  const int bRange = bMax - bMin;
  aValue = Clamp(aValue, aMin, aMax);
  /* 10-Гц единицы частоты × ширина экрана переполняют int (470 МГц × 240):
   * на таких диапазонах считаем в 64 бита (иначе индекс за массивом → fault).
   * Проверять нужно и умножение, и прибавление aRange/2: произведение может
   * влезть в int, а сумма с половиной диапазона — уже нет (104–439 МГц:
   * 8.9M × 239 + 16.7M > INT_MAX, результат уходил в минус и заворачивался
   * в uint8_t → «горизонтальная линия» через полэкрана). */
  int p;
  if (!__builtin_mul_overflow(aValue - aMin, bRange, &p) &&
      !__builtin_add_overflow(p, aRange / 2, &p))
    return p / aRange + bMin;
  return (int)(((int64_t)(aValue - aMin) * bRange + aRange / 2) / aRange) + bMin;
}

uint8_t DBm2S(int dbm, bool isUHF) {
  uint8_t i = 0;
  dbm *= -1;
  for (i = 0; i < 15; i++) {
    if (dbm >= rssi2s[isUHF][i]) {
      return i;
    }
  }
  return i;
}

int16_t Rssi2DBm(uint16_t rssi) { return (rssi >> 1) - 160; }
uint16_t DBm2Rssi(int16_t dbm) { return (dbm + 160) << 1; }

// applied x2 to prevent initial rounding
uint8_t Rssi2PX(uint16_t rssi, uint8_t pxMin, uint8_t pxMax) {
  return ConvertDomain(rssi - 320, -260, -120, pxMin, pxMax);
}

uint16_t Min(const uint16_t *array, size_t n) {
  if (array == NULL || n == 0) {
    return 0;
  }
  uint16_t min = array[0];
  for (size_t i = 1; i < n; ++i) {
    if (array[i] < min) {
      min = array[i];
    }
  }
  return min;
}

uint16_t Max(const uint16_t *array, size_t n) {
  if (array == NULL || n == 0) {
    return 0;
  }
  uint16_t max = array[0];
  for (size_t i = 1; i < n; ++i) {
    if (array[i] > max) {
      max = array[i];
    }
  }
  return max;
}

uint16_t Mean(const uint16_t *array, size_t n) {
  if (array == NULL || n == 0) {
    return 0;
  }
  uint32_t sum = array[0];
  for (size_t i = 1; i < n; ++i) {
    sum += array[i];
  }
  return sum / n;
}

uint32_t AdjustU(uint32_t val, uint32_t min, uint32_t max, int32_t inc) {
  if (inc > 0) {
    return val == max - inc ? min : val + inc;
  } else {
    return val > min ? val + inc : max + inc;
  }
}

uint32_t IncDecU(uint32_t val, uint32_t min, uint32_t max, bool inc) {
  return AdjustU(val, min, max, inc ? 1 : -1);
}

bool IsReadable(const char *name) { return name[0] >= 32 && name[0] < 127; }

SQL GetSql(uint8_t level) {
  SQL sq = {0, 0, 255, 255, 255, 255};
  if (level == 0) {
    return sq;
  }

  // sq.ro = ConvertDomain(level, 0, 10, 10, 180);
  sq.ro = ConvertDomain(level, 0, 10, 60, 180);
  sq.no = ConvertDomain(level, 0, 10, 64, 12);
  sq.go = ConvertDomain(level, 0, 10, 100, 0); // was 32, 6

  sq.rc = sq.ro - SQ_HYSTERESIS;
  sq.nc = sq.no + SQ_HYSTERESIS;
  sq.gc = sq.go + SQ_HYSTERESIS;
  return sq;
}

uint16_t Std(const uint16_t *data, size_t n) {
  if (data == NULL || n == 0)
    return 0;
  uint32_t sumDev = 0;
  for (size_t i = 0; i < n; ++i)
    sumDev += data[i] * data[i];
  return sqrt32(sumDev / n);
}

uint32_t DeltaF(uint32_t f1, uint32_t f2) {
  return f1 > f2 ? f1 - f2 : f2 - f1;
}

uint32_t RoundToStep(uint32_t f, uint32_t step) {
  uint32_t sd = f % step;
  if (sd > step / 2) {
    f += step - sd;
  } else {
    f -= sd;
  }
  return f;
}
