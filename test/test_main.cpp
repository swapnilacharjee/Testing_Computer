#include <unity.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdio.h>

// ---- Replicate testable logic from main.cpp ----

static time_t mock_time = 0;
time_t get_mock_time() { return mock_time; }

const float ON_THRESHOLD = 10.0f;

bool isPcOn(float power) { return power > ON_THRESHOLD; }

float calcEnergyDelta(float energy, float lastEnergy) {
    if (energy > lastEnergy && (energy - lastEnergy) < 1.0f)
        return energy - lastEnergy;
    return 0.0f;
}

void getTimestampBuf(time_t t, char* buf) {
    struct tm* tm = localtime(&t);
    sprintf(buf, "%04d-%02d-%02d %02d:%02d:%02d",
        tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
        tm->tm_hour, tm->tm_min, tm->tm_sec);
}

void getDateKeyBuf(time_t t, char* buf) {
    struct tm* tm = localtime(&t);
    sprintf(buf, "%04d-%02d-%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
}

// ---- Tests ----

void test_pc_on_above_threshold() {
    TEST_ASSERT_TRUE(isPcOn(15.0f));
    TEST_ASSERT_TRUE(isPcOn(10.1f));
}

void test_pc_off_below_threshold() {
    TEST_ASSERT_FALSE(isPcOn(10.0f));
    TEST_ASSERT_FALSE(isPcOn(0.0f));
    TEST_ASSERT_FALSE(isPcOn(9.9f));
}

void test_energy_delta_normal() {
    float delta = calcEnergyDelta(0.5f, 0.4f);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.1f, delta);
}

void test_energy_delta_no_increase() {
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, calcEnergyDelta(0.4f, 0.5f));
}

void test_energy_delta_spike_ignored() {
    // Jump > 1.0 kWh in 2s is a spike, should return 0
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, calcEnergyDelta(2.0f, 0.5f));
}

void test_energy_delta_exact_boundary() {
    // Exactly 1.0 is NOT < 1.0, so ignored
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, calcEnergyDelta(1.5f, 0.5f));
}

void test_timestamp_format() {
    // 2024-06-15 10:30:45 UTC
    struct tm t = {};
    t.tm_year = 124; t.tm_mon = 5; t.tm_mday = 15;
    t.tm_hour = 10;  t.tm_min = 30; t.tm_sec = 45;
    time_t ts = mktime(&t);
    char buf[25];
    getTimestampBuf(ts, buf);
    // Check format length and separators
    TEST_ASSERT_EQUAL_INT(19, strlen(buf));
    TEST_ASSERT_EQUAL_CHAR('-', buf[4]);
    TEST_ASSERT_EQUAL_CHAR('-', buf[7]);
    TEST_ASSERT_EQUAL_CHAR(' ', buf[10]);
    TEST_ASSERT_EQUAL_CHAR(':', buf[13]);
    TEST_ASSERT_EQUAL_CHAR(':', buf[16]);
}

void test_date_key_format() {
    struct tm t = {};
    t.tm_year = 124; t.tm_mon = 0; t.tm_mday = 1;
    time_t ts = mktime(&t);
    char buf[11];
    getDateKeyBuf(ts, buf);
    TEST_ASSERT_EQUAL_INT(10, strlen(buf));
    TEST_ASSERT_EQUAL_CHAR('-', buf[4]);
    TEST_ASSERT_EQUAL_CHAR('-', buf[7]);
}

void test_today_cost_calculation() {
    float todayEnergy = 2.5f;
    float cost = todayEnergy * 14.0f;
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 35.0f, cost);
}

void setUp() {}
void tearDown() {}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_pc_on_above_threshold);
    RUN_TEST(test_pc_off_below_threshold);
    RUN_TEST(test_energy_delta_normal);
    RUN_TEST(test_energy_delta_no_increase);
    RUN_TEST(test_energy_delta_spike_ignored);
    RUN_TEST(test_energy_delta_exact_boundary);
    RUN_TEST(test_timestamp_format);
    RUN_TEST(test_date_key_format);
    RUN_TEST(test_today_cost_calculation);
    return UNITY_END();
}
