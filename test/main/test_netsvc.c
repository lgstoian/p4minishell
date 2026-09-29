// SPDX-FileCopyrightText: 2026 Stoian Alexandru
// SPDX-License-Identifier: MIT
/**
 * @file test_netsvc.c
 * @brief Unit tests for the event-service pure core.
 *
 * Covers the MQTT 3.1.1 packet codec (mqtt_codec.h), the subscription topic
 * matcher, the reconnect backoff, and the outbox record framing (netsvc.h).
 * All headless: no sockets, no SD, no tasks. The socket driver, journal
 * files, and merge paths are hardware-verified through s16_netsvc.
 */
#include "unity.h"
#include "mqtt_codec.h"
#include "netsvc.h"

#include <string.h>

/* ========================================================================
 * Remaining-length varint
 * ======================================================================== */

void test_netsvc_remaining_roundtrip(void)
{
    uint8_t buf[4];
    uint32_t len = 0;
    size_t used = 0;

    TEST_ASSERT_EQUAL(1, mqtt_encode_remaining(0, buf));
    TEST_ASSERT_EQUAL(1, mqtt_decode_remaining(buf, 1, &len, &used));
    TEST_ASSERT_EQUAL(0, len);

    TEST_ASSERT_EQUAL(1, mqtt_encode_remaining(127, buf));
    TEST_ASSERT_EQUAL(2, mqtt_encode_remaining(128, buf));
    TEST_ASSERT_EQUAL(2, mqtt_encode_remaining(16383, buf));
    TEST_ASSERT_EQUAL(3, mqtt_encode_remaining(16384, buf));
    TEST_ASSERT_EQUAL(4, mqtt_encode_remaining(268435455u, buf));
    TEST_ASSERT_EQUAL(-1, mqtt_encode_remaining(268435456u, buf));

    TEST_ASSERT_EQUAL(1, mqtt_decode_remaining(buf, 4, &len, &used));
    TEST_ASSERT_EQUAL(268435455u, len);
    TEST_ASSERT_EQUAL(4, used);

    /* Truncated and overlong inputs fail closed. */
    {
        uint8_t cut[1] = {0x80};
        TEST_ASSERT_EQUAL(0, mqtt_decode_remaining(cut, 1, &len, &used));
    }
    {
        uint8_t bad[4] = {0xFF, 0xFF, 0xFF, 0xFF};
        TEST_ASSERT_EQUAL(-1, mqtt_decode_remaining(bad, 4, &len, &used));
    }
    TEST_ASSERT_EQUAL(-1, mqtt_encode_remaining(0, NULL));
    TEST_ASSERT_EQUAL(-1, mqtt_decode_remaining(NULL, 0, NULL, NULL));
}

/* ========================================================================
 * CONNECT / CONNACK
 * ======================================================================== */

void test_netsvc_connect_codec(void)
{
    uint8_t frame[128];
    mqtt_connect_args_t args;
    uint8_t type = 0;
    uint8_t flags = 0;
    uint32_t rem = 0;
    size_t hdrlen = 0;
    int n;
    memset(&args, 0, sizeof(args));
    args.client_id = "p4mini-test";
    args.keepalive_s = 60;
    args.clean_session = true;

    n = mqtt_encode_connect(frame, sizeof(frame), &args);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL(1, mqtt_decode_header(frame, (size_t)n, &type, &flags,
                                            &rem, &hdrlen));
    TEST_ASSERT_EQUAL(MQTT_PKT_CONNECT, type);
    TEST_ASSERT_EQUAL(0, flags);
    TEST_ASSERT_EQUAL((uint32_t)(n - (int)hdrlen), rem);

    /* Anonymous CONNECT carries no username flag (byte 7 of the body). */
    TEST_ASSERT_EQUAL(0, frame[hdrlen + 7] & 0xC0);

    /* Credentialed CONNECT sets both flags. */
    args.username = "user";
    args.password = "pass";
    n = mqtt_encode_connect(frame, sizeof(frame), &args);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL(0xC0, frame[hdrlen + 7] & 0xC0);

    /* Missing client id and tiny buffers refuse. */
    args.client_id = "";
    TEST_ASSERT_EQUAL(-1, mqtt_encode_connect(frame, sizeof(frame), &args));
    args.client_id = "x";
    TEST_ASSERT_EQUAL(-1, mqtt_encode_connect(frame, 4, &args));
    TEST_ASSERT_EQUAL(-1, mqtt_encode_connect(NULL, sizeof(frame), &args));

    /* CONNACK body. */
    {
        uint8_t ack[2] = {0x00, 0x00};
        uint8_t rc = 0xFF;
        TEST_ASSERT_EQUAL(0, mqtt_decode_connack(ack, sizeof(ack), &rc));
        TEST_ASSERT_EQUAL(MQTT_RC_ACCEPTED, rc);
        TEST_ASSERT_EQUAL(-1, mqtt_decode_connack(ack, 1, &rc));
    }
}

/* ========================================================================
 * PUBLISH / SUBSCRIBE / control frames
 * ======================================================================== */

void test_netsvc_publish_codec(void)
{
    uint8_t frame[128];
    const uint8_t hello[] = "hello";
    uint8_t type = 0;
    uint8_t flags = 0;
    uint32_t rem = 0;
    size_t hdrlen = 0;
    mqtt_view_t topic = {NULL, 0};
    mqtt_view_t payload = {NULL, 0};
    uint16_t pkt_id = 0;
    int n;

    n = mqtt_encode_publish(frame, sizeof(frame), "a/b", hello,
                            sizeof(hello) - 1, 1, 7, false);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL(1, mqtt_decode_header(frame, (size_t)n, &type, &flags,
                                            &rem, &hdrlen));
    TEST_ASSERT_EQUAL(MQTT_PKT_PUBLISH, type);
    TEST_ASSERT_EQUAL(0x02, flags & 0x06);
    TEST_ASSERT_EQUAL(0, mqtt_decode_publish(frame + hdrlen, rem, flags,
                                             &topic, &pkt_id, &payload));
    TEST_ASSERT_EQUAL(3, topic.len);
    TEST_ASSERT_EQUAL_MEMORY("a/b", topic.data, 3);
    TEST_ASSERT_EQUAL(7, pkt_id);
    TEST_ASSERT_EQUAL(sizeof(hello) - 1, payload.len);
    TEST_ASSERT_EQUAL_MEMORY("hello", payload.data, payload.len);

    /* QoS 0 carries no packet id; QoS 2 is refused (unsupported). */
    n = mqtt_encode_publish(frame, sizeof(frame), "a", hello, 5, 0, 0, false);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL(-1, mqtt_encode_publish(frame, sizeof(frame), "a",
                                              hello, 5, 2, 1, false));
    TEST_ASSERT_EQUAL(-1, mqtt_encode_publish(frame, sizeof(frame), "",
                                              hello, 5, 0, 0, false));

    /* PUBACK / PINGREQ / DISCONNECT shapes. */
    TEST_ASSERT_EQUAL(4, mqtt_encode_puback(frame, sizeof(frame), 0x1234));
    TEST_ASSERT_EQUAL((uint8_t)(MQTT_PKT_PUBACK << 4), frame[0]);
    TEST_ASSERT_EQUAL(0x12, frame[2]);
    TEST_ASSERT_EQUAL(2, mqtt_encode_pingreq(frame, sizeof(frame)));
    TEST_ASSERT_EQUAL(2, mqtt_encode_disconnect(frame, sizeof(frame)));
    TEST_ASSERT_EQUAL(-1, mqtt_encode_pingreq(frame, 1));
}

void test_netsvc_subscribe_codec(void)
{
    uint8_t frame[128];
    uint16_t pkt_id = 0;
    uint8_t granted[4];
    int n;
    int g;

    n = mqtt_encode_subscribe(frame, sizeof(frame), 9, "sensors/#", 1);
    TEST_ASSERT_GREATER_THAN(0, n);
    {
        uint8_t body[] = {0x00, 0x09, 0x01};
        g = mqtt_decode_suback(body, sizeof(body), &pkt_id, granted, sizeof(granted));
        TEST_ASSERT_EQUAL(1, g);
        TEST_ASSERT_EQUAL(9, pkt_id);
        TEST_ASSERT_EQUAL(1, granted[0]);
    }
    TEST_ASSERT_EQUAL(-1, mqtt_encode_subscribe(frame, sizeof(frame), 1, "", 0));
    TEST_ASSERT_EQUAL(-1, mqtt_encode_subscribe(frame, sizeof(frame), 1, "a", 2));

    n = mqtt_encode_unsubscribe(frame, sizeof(frame), 3, "sensors/#");
    TEST_ASSERT_GREATER_THAN(0, n);
}

/* ========================================================================
 * Topic matcher
 * ======================================================================== */

void test_netsvc_topic_matches(void)
{
    TEST_ASSERT_TRUE(mqtt_topic_matches("a/b", "a/b"));
    TEST_ASSERT_TRUE(mqtt_topic_matches("a/+", "a/b"));
    TEST_ASSERT_TRUE(mqtt_topic_matches("+/b", "a/b"));
    TEST_ASSERT_TRUE(mqtt_topic_matches("#", "a/b/c"));
    TEST_ASSERT_TRUE(mqtt_topic_matches("a/#", "a"));
    TEST_ASSERT_TRUE(mqtt_topic_matches("a/#", "a/b/c"));
    TEST_ASSERT_TRUE(mqtt_topic_matches("$pim/db/n", "$pim/db/n"));
    TEST_ASSERT_TRUE(mqtt_topic_matches("$pim/#", "$pim/alarms"));
    TEST_ASSERT_FALSE(mqtt_topic_matches("a/b", "a/c"));
    TEST_ASSERT_FALSE(mqtt_topic_matches("a/+", "a/b/c"));
    TEST_ASSERT_FALSE(mqtt_topic_matches("a/b", "a"));
    TEST_ASSERT_FALSE(mqtt_topic_matches("a", "a/b"));
    TEST_ASSERT_FALSE(mqtt_topic_matches("a/#/c", "a/b/c"));
    TEST_ASSERT_FALSE(mqtt_topic_matches(NULL, "a"));
    TEST_ASSERT_FALSE(mqtt_topic_matches("a", NULL));
}

/* ========================================================================
 * Backoff + outbox framing
 * ======================================================================== */

void test_netsvc_backoff(void)
{
    TEST_ASSERT_EQUAL(1000, netsvc_backoff_ms(0, 1000, 60000));
    TEST_ASSERT_EQUAL(2000, netsvc_backoff_ms(1, 1000, 60000));
    TEST_ASSERT_EQUAL(8000, netsvc_backoff_ms(3, 1000, 60000));
    TEST_ASSERT_EQUAL(60000, netsvc_backoff_ms(10, 1000, 60000));
    TEST_ASSERT_EQUAL(60000, netsvc_backoff_ms(100, 1000, 60000));
    TEST_ASSERT_EQUAL(0, netsvc_backoff_ms(0, 1000, 0));
}

void test_netsvc_outbox_codec(void)
{
    char rec[256];
    char topic[64];
    const uint8_t *payload = NULL;
    size_t paylen = 0;
    uint8_t qos = 0;
    const uint8_t hello[] = "hi";
    int n;

    n = netsvc_outbox_encode("a/b", hello, sizeof(hello) - 1, 1,
                             rec, sizeof(rec));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL(0, netsvc_outbox_decode(rec, (size_t)n, topic,
                                              sizeof(topic), &payload,
                                              &paylen, &qos));
    TEST_ASSERT_EQUAL_STRING("a/b", topic);
    TEST_ASSERT_EQUAL(sizeof(hello) - 1, paylen);
    TEST_ASSERT_EQUAL_MEMORY("hi", payload, paylen);
    TEST_ASSERT_EQUAL(1, qos);

    /* Empty payload round-trips; damage fails closed. */
    n = netsvc_outbox_encode("a", NULL, 0, 1, rec, sizeof(rec));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL(0, netsvc_outbox_decode(rec, (size_t)n, topic,
                                              sizeof(topic), &payload,
                                              &paylen, NULL));
    TEST_ASSERT_EQUAL(0, paylen);
    rec[0] = 'X';
    TEST_ASSERT_EQUAL(-1, netsvc_outbox_decode(rec, (size_t)n, topic,
                                               sizeof(topic), &payload,
                                               &paylen, NULL));
    TEST_ASSERT_EQUAL(-1, netsvc_outbox_decode("N1\n", 3, topic, sizeof(topic),
                                               &payload, &paylen, NULL));

    /* Argument validation. */
    TEST_ASSERT_EQUAL(-1, netsvc_outbox_encode(NULL, hello, 2, 1, rec, sizeof(rec)));
    TEST_ASSERT_EQUAL(-1, netsvc_outbox_encode("a", hello, 2, 2, rec, sizeof(rec)));
    TEST_ASSERT_EQUAL(-1, netsvc_outbox_encode("a", hello, 2, 1, rec, 8));
}
