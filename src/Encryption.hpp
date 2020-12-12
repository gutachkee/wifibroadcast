
#ifndef ENCRYPTION_HPP
#define ENCRYPTION_HPP

#include "wifibroadcast.hpp"
#include <stdio.h>
#include <stdexcept>
#include <vector>
#include <optional>
#include <iostream>
#include "Helper.hpp"

// For developing or when encryption is not important, you can use this default seed to
// create deterministic rx and tx keys
static const std::array<unsigned char,crypto_box_SEEDBYTES> DEFAULT_ENCRYPTION_SEED={0};
// use this one if you are worried about CPU usage when using encryption
//#define DO_NOT_ENCRYPT_DATA_BUT_PROVIDE_BACKWARDS_COMPABILITY

class Encryptor {
public:
    // enable a default deterministic encryption key by using std::nullopt
    // else, pass path to file with encryption keys
    explicit Encryptor(std::optional<std::string> keypair) {
        if(keypair==std::nullopt){
            // use default encryption keys
            crypto_box_seed_keypair(rx_publickey.data(),tx_secretkey.data(),DEFAULT_ENCRYPTION_SEED.data());
            std::cout<<"Using default keys\n";
        }else{
            FILE *fp;
            if ((fp = fopen(keypair->c_str(), "r")) == nullptr) {
                throw std::runtime_error(StringFormat::convert("Unable to open %s: %s", keypair->c_str(), strerror(errno)));
            }
            if (fread(tx_secretkey.data(), crypto_box_SECRETKEYBYTES, 1, fp) != 1) {
                fclose(fp);
                throw std::runtime_error(StringFormat::convert("Unable to read tx secret key: %s", strerror(errno)));
            }
            if (fread(rx_publickey.data(), crypto_box_PUBLICKEYBYTES, 1, fp) != 1) {
                fclose(fp);
                throw std::runtime_error(StringFormat::convert("Unable to read rx public key: %s", strerror(errno)));
            }
            fclose(fp);
        }
    }
    // Don't forget to send the session key after creating a new one
    void makeSessionKey() {
        randombytes_buf(session_key.data(), sizeof(session_key));
        randombytes_buf(sessionKeyPacket.session_key_nonce, sizeof(sessionKeyPacket.session_key_nonce));
        if (crypto_box_easy(sessionKeyPacket.session_key_data, session_key.data(), sizeof(session_key),
                            sessionKeyPacket.session_key_nonce, rx_publickey.data(), tx_secretkey.data()) != 0) {
            throw std::runtime_error("Unable to make session key!");
        }
    }
    // create a wfb packet by copying the header and
    // then putting the encrypted data right behind
    // the WBDataHeader is needed for calling the encryption method since it contains the 'nonce' for the message
    // TODO: Here the WBDataHeader is included as bytes in the returned array
    // For generalization, this should probably be seperated
    std::vector<uint8_t>
    makeEncryptedPacketIncludingHeader(const WBDataHeader& wblockHdr, const uint8_t* payload, std::size_t payloadSize) {
#ifdef DO_NOT_ENCRYPT_DATA_BUT_PROVIDE_BACKWARDS_COMPABILITY
        std::vector<uint8_t> ret;
        ret.resize(sizeof(WBDataHeader)+payloadSize+ crypto_aead_chacha20poly1305_ABYTES);
        memcpy(ret.data(),(uint8_t*)&wblockHdr,sizeof(WBDataHeader));
        memcpy(ret.data()+sizeof(WBDataHeader),payload,payloadSize);
        return ret;
#else
        std::vector<uint8_t> ret;
        ret.resize(sizeof(WBDataHeader)+payloadSize+ crypto_aead_chacha20poly1305_ABYTES);
        // copy the wblockHdr data (this part is not encrypted)
        memcpy(ret.data(),(uint8_t*)&wblockHdr,sizeof(WBDataHeader));
        // pointer to where the encrypted data begins
        uint8_t* cyphertext=&ret[sizeof(WBDataHeader)];
        long long unsigned int ciphertext_len;

        crypto_aead_chacha20poly1305_encrypt(cyphertext, &ciphertext_len,
                                             payload, payloadSize,
                                             (uint8_t *) &wblockHdr, sizeof(WBDataHeader),
                                             nullptr,
                                             (uint8_t *) (&(wblockHdr.nonce)), session_key.data());
        // we allocate the right size in the beginning, but check if ciphertext_len is actually matching what we calculated
        // (the documentation says 'write up to n bytes' but they probably mean (write n bytes if everything goes well)
        assert(ret.size()==sizeof(WBDataHeader)+ciphertext_len);
        return ret;
#endif
    }
    std::vector<uint8_t> makeEncryptedPacketIncludingHeader(const WBDataPacket& wbDataPacket) {
        return makeEncryptedPacketIncludingHeader(wbDataPacket.wbDataHeader, wbDataPacket.payload, wbDataPacket.payloadSize);
    }
    // encrypt the payload of the WBDataPacket
    /*WBDataPacket encryptWBDataPacket(const WBDataPacket& wbDataPacket){
        // we need to allocate a new buffer to also hold the crypto_aead_chacha20poly1305_ABYTES
        std::shared_ptr<std::vector<uint8_t>> encryptedData=std::make_unique<std::vector<uint8_t>>(wbDataPacket.payloadSize+ crypto_aead_chacha20poly1305_ABYTES);
#ifdef DO_NOT_ENCRYPT_DATA_BUT_PROVIDE_BACKWARDS_COMPABILITY
        return {wbDataPacket.wbDataHeader.nonce,wb};
#else
        // encryption method is c-style
        long long unsigned int ciphertext_len;
        crypto_aead_chacha20poly1305_encrypt(encryptedData->data(), &ciphertext_len,
                                             wbDataPacket.payload, wbDataPacket.payloadSize,
                                             (uint8_t *) &wbDataPacket.wbDataHeader, sizeof(WBDataHeader),
                                             nullptr,
                                             (uint8_t *) (&(wbDataPacket.wbDataHeader.nonce)), session_key.data());
        // we allocate the right size in the beginning, but check if ciphertext_len is actually matching what we calculated
        // (the documentation says 'write up to n bytes' but they probably mean (write n bytes if everything goes well)
        assert(encryptedData->size()==ciphertext_len);
        return {wbDataPacket.wbDataHeader.nonce, encryptedData};
#endif
    }*/
private:
    // tx->rx keypair
    std::array<uint8_t, crypto_box_SECRETKEYBYTES> tx_secretkey{};
    std::array<uint8_t, crypto_box_PUBLICKEYBYTES> rx_publickey{};
    std::array<uint8_t, crypto_aead_chacha20poly1305_KEYBYTES> session_key{};
public:
    // re-send this packet each time a new session key is created
    WBSessionKeyPacket sessionKeyPacket;
};

class Decryptor {
public:
    // enable a default deterministic encryption key by using std::nullopt
    // else, pass path to file with encryption keys
    explicit Decryptor(std::optional<std::string> keypair) {
        if(keypair==std::nullopt){
            crypto_box_seed_keypair(tx_publickey.data(),rx_secretkey.data(),DEFAULT_ENCRYPTION_SEED.data());
            std::cout<<"Using default keys\n";
        }else{
            FILE *fp;
            if ((fp = fopen(keypair->c_str(), "r")) == nullptr) {
                throw std::runtime_error(StringFormat::convert("Unable to open %s: %s", keypair->c_str(), strerror(errno)));
            }
            if (fread(rx_secretkey.data(), crypto_box_SECRETKEYBYTES, 1, fp) != 1) {
                fclose(fp);
                throw std::runtime_error(StringFormat::convert("Unable to read rx secret key: %s", strerror(errno)));
            }
            if (fread(tx_publickey.data(), crypto_box_PUBLICKEYBYTES, 1, fp) != 1) {
                fclose(fp);
                throw std::runtime_error(StringFormat::convert("Unable to read tx public key: %s", strerror(errno)));
            }
            fclose(fp);
        }
        memset(session_key.data(), '\0', sizeof(session_key));
    }
public:
    std::array<uint8_t, crypto_box_SECRETKEYBYTES> rx_secretkey{};
public:
    std::array<uint8_t, crypto_box_PUBLICKEYBYTES> tx_publickey{};
    std::array<uint8_t, crypto_aead_chacha20poly1305_KEYBYTES> session_key{};
public:
    // return true if a new session was detected (The same session key can be sent multiple times by the tx)
    bool onNewPacketWfbKey(const uint8_t *buf) {
        std::array<uint8_t, sizeof(session_key)> new_session_key{};
        const WBSessionKeyPacket* sessionKeyPacket=(WBSessionKeyPacket*)buf;
        if (crypto_box_open_easy(new_session_key.data(),
                                 sessionKeyPacket->session_key_data, sizeof(WBSessionKeyPacket::session_key_data),
                                 sessionKeyPacket->session_key_nonce,
                                 tx_publickey.data(), rx_secretkey.data()) != 0) {
            // this basically should just never happen
            std::cerr<<"unable to decrypt session key\n";
            return false;
        }
        if (memcmp(session_key.data(), new_session_key.data(), sizeof(session_key)) != 0) {
            // this is NOT an error
            std::cerr<<"New session detected\n";
            session_key = new_session_key;
            return true;
        }
        return false;
    }

    // returns decrypted data on success
    std::optional<std::vector<uint8_t>> decryptPacket(const WBDataHeader& wblockHdr,const uint8_t* encryptedPayload,std::size_t encryptedPayloadSize) {
#ifdef DO_NOT_ENCRYPT_DATA_BUT_PROVIDE_BACKWARDS_COMPABILITY
        return std::vector<uint8_t>(encryptedPayload,encryptedPayload+(encryptedPayloadSize-crypto_aead_chacha20poly1305_ABYTES));
#else
        std::vector<uint8_t> decrypted;
        decrypted.resize(encryptedPayloadSize-crypto_aead_chacha20poly1305_ABYTES);

        long long unsigned int decrypted_len;
        const unsigned long long int cLen=encryptedPayloadSize;

        if (crypto_aead_chacha20poly1305_decrypt(decrypted.data(), &decrypted_len,
                                                 nullptr,
                                                 encryptedPayload,cLen,
                                                 (uint8_t*)&wblockHdr,sizeof(WBDataHeader),
                                                 (uint8_t *) (&(wblockHdr.nonce)), session_key.data()) != 0) {
            return std::nullopt;
        }
        assert(decrypted.size()==decrypted_len);
        return decrypted;
#endif
    }
};

#endif //ENCRYPTION_HPP