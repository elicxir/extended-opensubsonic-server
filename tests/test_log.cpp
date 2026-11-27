#include <gtest/gtest.h>

#include "core/log.h"

using eos::log::redact;

TEST(Log, RedactsCredentialParameters) {
  EXPECT_EQ(redact("GET /rest/ping.view?u=alice&t=abc123&s=salt&v=1.16.1&c=x"),
            "GET /rest/ping.view?u=alice&t=***&s=***&v=1.16.1&c=x");
  EXPECT_EQ(redact("/rest/stream?id=1&apiKey=eos_secret"), "/rest/stream?id=1&apiKey=***");
  EXPECT_EQ(redact("p=enc:6869&f=json"), "p=***&f=json");
  EXPECT_EQ(redact("/rest/getSong?id=st&sort=t"), "/rest/getSong?id=st&sort=t");  // names, not values
}
