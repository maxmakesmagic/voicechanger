#ifndef TEST_SUPPORT_ARDUINO_H_
#define TEST_SUPPORT_ARDUINO_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>

#ifndef TWO_PI
#define TWO_PI 6.283185307179586476925286766559f
#endif

namespace arduino_test {

inline unsigned long currentMilliseconds = 0;

inline void setMilliseconds(unsigned long milliseconds)
{
  currentMilliseconds = milliseconds;
}

inline void resetClock()
{
  currentMilliseconds = 0;
}

} // namespace arduino_test

inline unsigned long millis()
{
  return arduino_test::currentMilliseconds;
}

// Host implementation of Teensy's elapsedMillis utility. Tests advance its
// clock explicitly through arduino_test::setMilliseconds().
class elapsedMillis {
public:
  elapsedMillis() : startedAt_(millis()) {}
  elapsedMillis(unsigned long value) : startedAt_(millis() - value) {}

  operator unsigned long() const
  {
    return millis() - startedAt_;
  }

  elapsedMillis &operator=(unsigned long value)
  {
    startedAt_ = millis() - value;
    return *this;
  }

private:
  unsigned long startedAt_;
};

// Relevant subset of Arduino's Print API. Its typed formatting methods are
// deliberately non-virtual: like the real implementation, they ultimately
// dispatch bytes through virtual write().
class Print {
public:
  virtual ~Print() = default;
  virtual std::size_t write(uint8_t byte) = 0;

  virtual std::size_t write(const uint8_t *buffer, std::size_t size)
  {
    std::size_t written = 0;
    while (size-- != 0) {
      written += write(*buffer++);
    }
    return written;
  }

  std::size_t write(const char *text)
  {
    if (text == nullptr) {
      return 0;
    }
    return write(reinterpret_cast<const uint8_t *>(text), std::strlen(text));
  }

  std::size_t print(const char text[])
  {
    return write(text);
  }

  std::size_t print(unsigned int value)
  {
    const std::string formatted = std::to_string(value);
    return write(formatted.c_str());
  }

  std::size_t print(double value, int digits = 2)
  {
    std::ostringstream formatted;
    formatted << std::fixed << std::setprecision(digits) << value;
    return write(formatted.str().c_str());
  }

  std::size_t println()
  {
    const uint8_t newline[] = {'\r', '\n'};
    return write(newline, sizeof(newline));
  }

  std::size_t println(const char text[])
  {
    return print(text) + println();
  }
};

class NullPrint final : public Print {
public:
  std::size_t write(uint8_t) override { return 1; }
};

inline NullPrint Serial;

#endif
