#include "crypto/aes_cmac.h"
#include "crypto/brainpoolp256r1.h"
#include "crypto/sha1.h"
#include "runtime.h"
#include "usb_descriptor.h"

static int bytes_equal(const unsigned char *left, const unsigned char *right, size_t length) {
    return memcmp(left, right, length) == 0;
}

static int test_sha1(void) {
    static const unsigned char expected[CRYPTO_SHA1_DIGEST_SIZE] = {
        0xa9U, 0x99U, 0x3eU, 0x36U, 0x47U, 0x06U, 0x81U, 0x6aU,
        0xbaU, 0x3eU, 0x25U, 0x71U, 0x78U, 0x50U, 0xc2U, 0x6cU,
        0x9cU, 0xd0U, 0xd8U, 0x9dU
    };
    unsigned char digest[CRYPTO_SHA1_DIGEST_SIZE];

    crypto_sha1_hash((const unsigned char *)"abc", 3U, digest);
    return bytes_equal(digest, expected, sizeof(expected)) ? 0 : 1;
}

static int test_aes_cmac(void) {
    static const unsigned char key[16] = {
        0x2bU, 0x7eU, 0x15U, 0x16U, 0x28U, 0xaeU, 0xd2U, 0xa6U,
        0xabU, 0xf7U, 0x15U, 0x88U, 0x09U, 0xcfU, 0x4fU, 0x3cU
    };
    static const unsigned char block[16] = {
        0x6bU, 0xc1U, 0xbeU, 0xe2U, 0x2eU, 0x40U, 0x9fU, 0x96U,
        0xe9U, 0x3dU, 0x7eU, 0x11U, 0x73U, 0x93U, 0x17U, 0x2aU
    };
    static const unsigned char expected_empty[CRYPTO_AES_CMAC_SIZE] = {
        0xbbU, 0x1dU, 0x69U, 0x29U, 0xe9U, 0x59U, 0x37U, 0x28U,
        0x7fU, 0xa3U, 0x7dU, 0x12U, 0x9bU, 0x75U, 0x67U, 0x46U
    };
    static const unsigned char expected_block[CRYPTO_AES_CMAC_SIZE] = {
        0x07U, 0x0aU, 0x16U, 0xb4U, 0x6bU, 0x4dU, 0x41U, 0x44U,
        0xf7U, 0x9bU, 0xddU, 0x9dU, 0xd0U, 0x4aU, 0x28U, 0x7cU
    };
    unsigned char mac[CRYPTO_AES_CMAC_SIZE];

    if (crypto_aes128_cmac(key, 0, 0U, mac) != 0 || !bytes_equal(mac, expected_empty, sizeof(expected_empty))) return 2;
    if (crypto_aes128_cmac(key, block, sizeof(block), mac) != 0 || !bytes_equal(mac, expected_block, sizeof(expected_block))) return 3;
    return 0;
}

static int test_brainpool(void) {
    static const unsigned char expected_public[CRYPTO_BRAINPOOLP256R1_PUBLIC_KEY_SIZE] = {
        0x04U,
        0x8bU, 0xd2U, 0xaeU, 0xb9U, 0xcbU, 0x7eU, 0x57U, 0xcbU,
        0x2cU, 0x4bU, 0x48U, 0x2fU, 0xfcU, 0x81U, 0xb7U, 0xafU,
        0xb9U, 0xdeU, 0x27U, 0xe1U, 0xe3U, 0xbdU, 0x23U, 0xc2U,
        0x3aU, 0x44U, 0x53U, 0xbdU, 0x9aU, 0xceU, 0x32U, 0x62U,
        0x54U, 0x7eU, 0xf8U, 0x35U, 0xc3U, 0xdaU, 0xc4U, 0xfdU,
        0x97U, 0xf8U, 0x46U, 0x1aU, 0x14U, 0x61U, 0x1dU, 0xc9U,
        0xc2U, 0x77U, 0x45U, 0x13U, 0x2dU, 0xedU, 0x8eU, 0x54U,
        0x5cU, 0x1dU, 0x54U, 0xc7U, 0x2fU, 0x04U, 0x69U, 0x97U
    };
    unsigned char private_key[CRYPTO_BRAINPOOLP256R1_SCALAR_SIZE];
    unsigned char public_key[CRYPTO_BRAINPOOLP256R1_PUBLIC_KEY_SIZE];

    rt_memset(private_key, 0, sizeof(private_key));
    private_key[sizeof(private_key) - 1U] = 1U;
    if (!crypto_brainpoolp256r1_public_from_private(private_key, public_key)) return 4;
    if (!bytes_equal(public_key, expected_public, sizeof(expected_public))) return 5;
    if (!crypto_brainpoolp256r1_public_key_valid(public_key)) return 6;
    public_key[0] = 0x05U;
    if (crypto_brainpoolp256r1_public_key_valid(public_key)) return 7;
    return 0;
}

static int test_usb_descriptors(void) {
    static const unsigned char device_descriptor[18] = {
        18U, USB_DESCRIPTOR_TYPE_DEVICE,
        0x00U, 0x02U,
        0x00U, 0x00U, 0x00U, 64U,
        0x34U, 0x12U,
        0x78U, 0x56U,
        0x00U, 0x01U,
        1U, 2U, 3U, 1U
    };
    static const unsigned char configuration[] = {
        9U, USB_DESCRIPTOR_TYPE_CONFIGURATION, 44U, 0U, 1U, 1U, 0U, 0x80U, 50U,
        9U, USB_DESCRIPTOR_TYPE_INTERFACE, 0U, 0U, 3U, USB_CLASS_SMART_CARD, 0U, 0U, 0U,
        5U, USB_DESCRIPTOR_TYPE_CCID, 0x10U, 0x01U, 0U,
        7U, USB_DESCRIPTOR_TYPE_ENDPOINT, 0x02U, USB_ENDPOINT_TRANSFER_TYPE_BULK, 64U, 0U, 0U,
        7U, USB_DESCRIPTOR_TYPE_ENDPOINT, 0x81U, USB_ENDPOINT_TRANSFER_TYPE_BULK, 64U, 0U, 0U,
        7U, USB_DESCRIPTOR_TYPE_ENDPOINT, 0x83U, USB_ENDPOINT_TRANSFER_TYPE_INTERRUPT, 8U, 0U, 10U
    };
    UsbDeviceDescriptor device;
    UsbConfigurationDescriptor config;
    UsbCcidInterface ccid;

    if (usb_parse_device_descriptor(device_descriptor, sizeof(device_descriptor), &device) != 0) return 8;
    if (device.vendor_id != 0x1234U || device.product_id != 0x5678U || device.configuration_count != 1U) return 9;
    if (usb_parse_configuration_descriptor(configuration, sizeof(configuration), &config) != 0) return 10;
    if (config.total_length != sizeof(configuration) || config.max_power_ma != 100U) return 11;
    if (usb_find_ccid_interface(configuration, sizeof(configuration), &ccid) != 0) return 12;
    if (ccid.interface_descriptor.number != 0U || ccid.bulk_out.address != 0x02U || ccid.bulk_in.address != 0x81U) return 13;
    if (!ccid.has_interrupt_in || ccid.interrupt_in.address != 0x83U || ccid.class_descriptor_length != 5U) return 14;
    return 0;
}

int main(void) {
    int result;

    result = test_sha1();
    if (result != 0) return result;
    result = test_aes_cmac();
    if (result != 0) return result;
    result = test_brainpool();
    if (result != 0) return result;
    result = test_usb_descriptors();
    if (result != 0) return result;
    return 0;
}