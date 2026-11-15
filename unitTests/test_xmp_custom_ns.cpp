// SPDX-License-Identifier: GPL-2.0-or-later
//
// Tests for custom (application-specific) XMP namespaces: parsing a packet
// that declares a namespace Exiv2 does not know about, writing it back
// faithfully, round-tripping the serialized result, and keeping the XMP
// Toolkit's internal namespace registry consistent across the lifetime of a
// process.

#include <gtest/gtest.h>
#include <exiv2/exiv2.hpp>
#include "unittest_utils.hpp"

#include <regex>
#include <string>

namespace {

using Exiv2::XmpData;
using Exiv2::XmpParser;
using Exiv2::XmpProperties;

constexpr const char* kNsUri = "https://custom.example.com/xmp/acme/1.0/";
constexpr const char* kNsPrefix = "acmeUnit";

// Build a minimal valid XMP packet binding kNsPrefix to kNsUri with one
// simple text property.
std::string packetWith(const std::string& tag, const std::string& value) {
  std::string packet = R"(<?xpacket begin="" id="W5M0MpCehiHzreSzNTczkc9d"?>)"
                       R"(<x:xmpmeta xmlns:x="adobe:ns:meta/">)"
                       R"(<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">)"
                       R"(<rdf:Description rdf:about="")";
  packet += std::string(" xmlns:") + kNsPrefix + "=\"" + kNsUri + "\" ";
  packet += std::string(kNsPrefix) + ":" + tag + "=\"" + value + "\"/>";
  packet += R"(</rdf:RDF></x:xmpmeta><?xpacket end="w"?>)";
  return packet;
}

// Return the URI declared for an xmlns prefix in a serialized packet.
std::string uriForPrefix(const std::string& packet, const std::string& prefix) {
  std::regex re("xmlns:" + prefix + R"RE(\s*=\s*"([^"]*)")RE");
  std::smatch match;
  return std::regex_search(packet, match, re) ? match[1].str() : std::string{};
}

// A bag (unordered array) in a custom namespace.
std::string bagPacket() {
  std::string packet = R"(<?xpacket begin="" id="W5M0MpCehiHzreSzNTczkc9d"?>)"
                       R"(<x:xmpmeta xmlns:x="adobe:ns:meta/">)"
                       R"(<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">)"
                       R"(<rdf:Description rdf:about="")";
  packet += std::string(" xmlns:") + kNsPrefix + "=\"" + kNsUri + "\">";
  packet += std::string("<") + kNsPrefix +
            ":Tags><rdf:Bag><rdf:li>alpha</rdf:li><rdf:li>beta</rdf:li></rdf:Bag></" + kNsPrefix +
            ":Tags>";
  packet += R"(</rdf:Description></rdf:RDF></x:xmpmeta><?xpacket end="w"?>)";
  return packet;
}

// Register the custom namespace up front, as an application would, and remove
// it again after the test so global state does not leak between test cases.
class CustomNamespace : public testing::Test {
 protected:
  void SetUp() override {
    XmpProperties::registerNs(kNsUri, kNsPrefix);
  }
  void TearDown() override {
    XmpParser::clearCustomNamespaces();
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// A packet declaring an unknown namespace is parsed into prefix-based keys and
// its values are exposed through the regular XmpData interface.
// ---------------------------------------------------------------------------
TEST_F(CustomNamespace, DecodesUnknownNamespaceIntoKeys) {
  XmpData xmp;
  ASSERT_EQ(0, XmpParser::decode(xmp, packetWith("ProjectId", "PRJ-42"), defaultDecodeParams()));
  ASSERT_EQ(1, xmp.count());

  const auto& datum = *xmp.begin();
  EXPECT_EQ(std::string("Xmp.") + kNsPrefix + ".ProjectId", datum.key());
  EXPECT_EQ("PRJ-42", datum.value().toString());
  EXPECT_EQ(kNsUri, XmpProperties::ns(kNsPrefix));
}

// ---------------------------------------------------------------------------
// Encoding the decoded data reproduces the original prefix-to-URI binding, so
// the custom namespace survives a write-back.
// ---------------------------------------------------------------------------
TEST_F(CustomNamespace, EncodesWithOriginalNamespaceUri) {
  XmpData xmp;
  ASSERT_EQ(0, XmpParser::decode(xmp, packetWith("ProjectId", "PRJ-42"), defaultDecodeParams()));

  std::string packet;
  ASSERT_EQ(0, XmpParser::encode(packet, xmp, XmpParser::omitPacketWrapper));
  EXPECT_EQ(kNsUri, uriForPrefix(packet, kNsPrefix));
  EXPECT_NE(std::string::npos, packet.find("PRJ-42"));
}

// ---------------------------------------------------------------------------
// A value modified after parsing is serialized under the same custom
// namespace, i.e. read-modify-write round trips.
// ---------------------------------------------------------------------------
TEST_F(CustomNamespace, ReadModifyWriteRoundTrips) {
  XmpData xmp;
  ASSERT_EQ(0, XmpParser::decode(xmp, packetWith("ProjectId", "PRJ-42"), defaultDecodeParams()));
  xmp[std::string("Xmp.") + kNsPrefix + ".ProjectId"] = "PRJ-43";

  std::string packet;
  ASSERT_EQ(0, XmpParser::encode(packet, xmp, XmpParser::omitPacketWrapper));
  EXPECT_EQ(kNsUri, uriForPrefix(packet, kNsPrefix));

  XmpData reread;
  ASSERT_EQ(0, XmpParser::decode(reread, packet, defaultDecodeParams()));
  ASSERT_EQ(1, reread.count());
  EXPECT_EQ("PRJ-43", reread.begin()->value().toString());
}

// ---------------------------------------------------------------------------
// Container value types (bags) in a custom namespace keep their type and
// contents through an encode/decode cycle.
// ---------------------------------------------------------------------------
TEST_F(CustomNamespace, PreservesBagValues) {
  XmpData xmp;
  ASSERT_EQ(0, XmpParser::decode(xmp, bagPacket(), defaultDecodeParams()));
  ASSERT_EQ(1, xmp.count());
  EXPECT_EQ(Exiv2::xmpBag, xmp.begin()->typeId());
  EXPECT_EQ(2, xmp.begin()->value().count());

  std::string packet;
  ASSERT_EQ(0, XmpParser::encode(packet, xmp, XmpParser::omitPacketWrapper));
  EXPECT_NE(std::string::npos, packet.find("rdf:Bag"));
  EXPECT_EQ(kNsUri, uriForPrefix(packet, kNsPrefix));

  XmpData reread;
  ASSERT_EQ(0, XmpParser::decode(reread, packet, defaultDecodeParams()));
  ASSERT_EQ(1, reread.count());
  EXPECT_EQ(Exiv2::xmpBag, reread.begin()->typeId());
  EXPECT_EQ(2, reread.begin()->value().count());
}

// ---------------------------------------------------------------------------
// Copying an XmpData object (a common pattern when moving metadata between
// images) preserves the per-instance namespace binding.
// ---------------------------------------------------------------------------
TEST_F(CustomNamespace, CopyPreservesNamespaceBinding) {
  XmpData original;
  ASSERT_EQ(0, XmpParser::decode(original, packetWith("ProjectId", "PRJ-42"), defaultDecodeParams()));

  XmpData copy = original;
  std::string packet;
  ASSERT_EQ(0, XmpParser::encode(packet, copy, XmpParser::omitPacketWrapper));
  EXPECT_EQ(kNsUri, uriForPrefix(packet, kNsPrefix));
}

// ---------------------------------------------------------------------------
// A property added programmatically after an explicit registerNs() is
// serialized under the registered custom namespace.
// ---------------------------------------------------------------------------
TEST_F(CustomNamespace, ProgrammaticPropertyUsesRegisteredUri) {
  XmpData xmp;
  xmp[std::string("Xmp.") + kNsPrefix + ".FreshProp"] = "hello";

  std::string packet;
  ASSERT_EQ(0, XmpParser::encode(packet, xmp, XmpParser::omitPacketWrapper));
  EXPECT_EQ(kNsUri, uriForPrefix(packet, kNsPrefix));
  EXPECT_NE(std::string::npos, packet.find("hello"));
}

// ---------------------------------------------------------------------------
// Regression: rebinding a prefix to a different URI after
// clearCustomNamespaces() must leave the XMP Toolkit's two internal namespace
// maps consistent. Previously RegisterNamespace() overwrote only the
// prefix->URI map, leaving a stale URI->prefix entry that made a later
// registeredNamespaces() (DumpNamespaces) throw an internal XMP_Error and
// abort the process.
// ---------------------------------------------------------------------------
TEST_F(CustomNamespace, RebindPrefixKeepsToolkitRegistryConsistent) {
  const std::string otherUri = "https://custom.example.com/xmp/acme/2.0/";

  // Exercise the registration through an encode so the toolkit maps are
  // populated.
  {
    XmpData xmp;
    xmp[std::string("Xmp.") + kNsPrefix + ".Foo"] = "first";
    std::string packet;
    ASSERT_EQ(0, XmpParser::encode(packet, xmp));
  }

  XmpParser::clearCustomNamespaces();
  XmpProperties::registerNs(otherUri, kNsPrefix);

  {
    XmpData xmp;
    xmp[std::string("Xmp.") + kNsPrefix + ".Foo"] = "second";
    std::string packet;
    ASSERT_EQ(0, XmpParser::encode(packet, xmp, XmpParser::omitPacketWrapper));
    EXPECT_EQ(otherUri, uriForPrefix(packet, kNsPrefix));
    EXPECT_EQ(std::string::npos, packet.find(kNsUri));
  }

  // This used to throw "Fatal namespace map problem" and abort the process:
  // rebinding the prefix overwrote the toolkit's prefix->URI map but left the
  // stale URI->prefix entry, so DumpNamespaces() detected inconsistent maps.
  Exiv2::Dictionary namespaces;
  EXPECT_NO_THROW(XmpProperties::registeredNamespaces(namespaces));

  // The Exiv2-level registry reflects the new binding.
  EXPECT_EQ(otherUri, XmpProperties::ns(kNsPrefix));
}

// ---------------------------------------------------------------------------
// Two independent packets that bind the same prefix to different URIs each
// serialize their own binding. Prefix-to-URI bindings are captured per
// XmpData instance at decode time, so neither image's output is poisoned by
// the other packet's binding (the global registry can only hold one URI for
// a given prefix).
// ---------------------------------------------------------------------------
TEST_F(CustomNamespace, SamePrefixDifferentUrisStayIndependentAndConsistent) {
  auto makePacket = [](const std::string& uri) {
    std::string packet = R"(<?xpacket begin="" id="W5M0MpCehiHzreSzNTczkc9d"?>)"
                         R"(<x:xmpmeta xmlns:x="adobe:ns:meta/">)"
                         R"(<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">)"
                         R"(<rdf:Description rdf:about=")"
                         R"(" xmlns:shared=")";
    packet += uri + "\" shared:Tag=\"v\"/>";
    packet += R"(</rdf:RDF></x:xmpmeta><?xpacket end="w"?>)";
    return packet;
  };
  const std::string uriA = "https://custom.example.com/shared/A/1.0/";
  const std::string uriB = "https://custom.example.com/shared/B/1.0/";

  XmpData a;
  XmpData b;
  ASSERT_EQ(0, XmpParser::decode(a, makePacket(uriA), defaultDecodeParams()));
  ASSERT_EQ(0, XmpParser::decode(b, makePacket(uriB), defaultDecodeParams()));

  std::string outA;
  std::string outB;
  ASSERT_EQ(0, XmpParser::encode(outA, a, XmpParser::omitPacketWrapper));
  ASSERT_EQ(0, XmpParser::encode(outB, b, XmpParser::omitPacketWrapper));
  EXPECT_EQ(uriA, uriForPrefix(outA, "shared"));
  EXPECT_EQ(uriB, uriForPrefix(outB, "shared"));
}
