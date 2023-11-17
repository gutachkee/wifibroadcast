#ifndef KEY_HPP
#define KEY_HPP

namespace wb {

// A wb key consists of a public and secret key
struct Key {
  std::array<uint8_t, crypto_box_PUBLICKEYBYTES> public_key;
  std::array<uint8_t, crypto_box_SECRETKEYBYTES> secret_key;
};

}  // namespace wb

#endif // KEY_HPP
