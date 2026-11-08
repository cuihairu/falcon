// XML scanner + WebDAV PROPFIND parser edge-case coverage.
// Exercises the pure header-only functions in src/xml_scan.hpp and
// plugins/webdav/webdav_propfind_xml.hpp without any network I/O.

#include "xml_scan.hpp"
#include "webdav_propfind_xml.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

using falcon::detail::XmlElement;
using falcon::detail::xml_find_element;
using falcon::detail::xml_local_name;
using falcon::detail::xml_decode_entities;
using falcon::detail::parse_dav_propfind_response;
using falcon::detail::parse_dav_quota;
using falcon::detail::DavEntry;
using falcon::detail::href_to_path;
using falcon::detail::percent_decode;
using falcon::detail::prop_field;

TEST(XmlScanCoverageTest, LocalNameStripsNamespacePrefix) {
    EXPECT_EQ(xml_local_name("d:getcontentlength"), "getcontentlength");
    EXPECT_EQ(xml_local_name("D:resourcetype"), "resourcetype");
    EXPECT_EQ(xml_local_name("plain"), "plain");
    EXPECT_EQ(xml_local_name("a:b:c"), "c");
    EXPECT_EQ(xml_local_name(""), "");
}

TEST(XmlScanCoverageTest, DecodeEntityMatrix) {
    EXPECT_EQ(xml_decode_entities("a&lt;b&gt;c&amp;d&quot;e&apos;f"),
              "a<b>c&d\"e'f");
    EXPECT_EQ(xml_decode_entities("x&#65;y&#x41;z&#X41;"), "xAyAzA");
    EXPECT_EQ(xml_decode_entities("&unknown;"), "&unknown;");
    EXPECT_EQ(xml_decode_entities("&gt;"), ">");
    EXPECT_EQ(xml_decode_entities("&#0;"), "&#0;");
    EXPECT_EQ(xml_decode_entities("&#127;"), std::string(1, '\x7F'));
    EXPECT_EQ(xml_decode_entities("&#128;"), "&#128;");
    EXPECT_EQ(xml_decode_entities("&#x1F600;"), "&#x1F600;");
    EXPECT_EQ(xml_decode_entities("&#99999999999999999999;"), "&#99999999999999999999;");
    EXPECT_EQ(xml_decode_entities("&abcdefghijklmnop;"), "&abcdefghijklmnop;");
    EXPECT_EQ(xml_decode_entities("&a"), "&a");
    EXPECT_EQ(xml_decode_entities("&&amp;"), "&&amp;");
    EXPECT_EQ(xml_decode_entities(""), "");
}

TEST(XmlScanCoverageTest, FindElementBranches) {
    XmlElement out{};

    // Empty input / no '<'
    EXPECT_FALSE(xml_find_element("", 0, "x", &out));
    EXPECT_FALSE(xml_find_element("no-tags-here", 0, "x", &out));
    // Trailing '<' with nothing after it
    EXPECT_FALSE(xml_find_element("<", 0, "x", &out));
    // Comment / CDATA / PI / closing tags are skipped
    EXPECT_TRUE(xml_find_element("<!-- a --><x>1</x>", 0, "x", &out));
    EXPECT_EQ(out.text, "1");
    EXPECT_TRUE(xml_find_element("<![CDATA[z]]><x>2</x>", 0, "x", &out));
    EXPECT_EQ(out.text, "2");
    EXPECT_TRUE(xml_find_element("<?pi?><x>3</x>", 0, "x", &out));
    EXPECT_EQ(out.text, "3");
    // Unclosed element
    EXPECT_FALSE(xml_find_element("<x>abc", 0, "x", &out));
    EXPECT_FALSE(xml_find_element("<x", 0, "x", &out));
    // Different tag before target
    EXPECT_TRUE(xml_find_element("<y>1</y><x>4</x>", 0, "x", &out));
    EXPECT_EQ(out.text, "4");
    // Self-closing
    EXPECT_TRUE(xml_find_element("<x/>tail", 0, "x", &out));
    EXPECT_EQ(out.text, "");
    EXPECT_EQ(out.next, 4u);
    // Attribute containing '>' must not terminate the tag
    EXPECT_TRUE(xml_find_element(R"(<x a="1>2">body</x>)", 0, "x", &out));
    EXPECT_EQ(out.text, "body");
    // Attribute with a bare apostrophe and escaped quote is not special-cased
    // by the minimal scanner: the first unescaped " ends the quote state, so
    // this is an attribute-only branch that still yields a match.
    EXPECT_TRUE(xml_find_element(R"(<x a='b'>q</x>)", 0, "x", &out));
    EXPECT_EQ(out.text, "q");
    // Search from a later offset
    EXPECT_TRUE(xml_find_element("<a>1</a><x>5</x>", 6, "x", &out));
    EXPECT_EQ(out.text, "5");
    // No more elements from offset
    EXPECT_FALSE(xml_find_element("<x>1</x>", 5, "x", &out));
    // Missing close for target name
    EXPECT_FALSE(xml_find_element("<x>1</y>", 0, "x", &out));
}

TEST(XmlScanCoverageTest, DavPercentDecodeAndHrefToPath) {
    EXPECT_EQ(percent_decode(""), "");
    EXPECT_EQ(percent_decode("%41%42"), "AB");
    EXPECT_EQ(percent_decode("plain"), "plain");
    EXPECT_EQ(percent_decode("%"), "%");
    EXPECT_EQ(percent_decode("%2"), "%2");
    EXPECT_EQ(percent_decode("%G1"), "%G1");
    EXPECT_EQ(percent_decode("a+b"), "a+b");

    EXPECT_EQ(href_to_path("/dav/docs/read%20me.txt"), "/dav/docs/read me.txt");
    EXPECT_EQ(href_to_path("https://host:8443/dav/a%2Fb?q=1"), "/dav/a/b");
    EXPECT_EQ(href_to_path("https://host/path"), "/path");
    EXPECT_EQ(href_to_path("https://host"), "/");
    EXPECT_EQ(href_to_path("relative/path"), "relative/path");
    EXPECT_EQ(href_to_path("https://host/a?x=1#frag"), "/a");
}

TEST(XmlScanCoverageTest, PropFieldBoundsAndMissing) {
    XmlElement prop{};
    prop.content_begin = 0;
    prop.content_end = 30;
    EXPECT_TRUE(xml_find_element(
        "<prop><a>1</a><b>2</b><c>3</c></prop>", 0, "prop", &prop));
    EXPECT_EQ(prop_field("<prop><a>1</a></prop>", prop, "a"), "1");
    // Field extends beyond prop end => rejected
    XmlElement small_prop{};
    small_prop.content_begin = 0;
    small_prop.content_end = 4;
    EXPECT_EQ(prop_field("<prop><a>123456</a></prop>", small_prop, "a"), "");
    EXPECT_EQ(prop_field("<prop><a>1</a></prop>", prop, "missing"), "");
}

TEST(XmlScanCoverageTest, DavPropfindResponseEdgeCases) {
    std::vector<DavEntry> entries;

    // Not a multistatus document => false
    EXPECT_FALSE(parse_dav_propfind_response("<html></html>", &entries));

    // Empty multistatus
    EXPECT_TRUE(parse_dav_propfind_response(
        "<d:multistatus xmlns:d=\"DAV:\"></d:multistatus>", &entries));
    EXPECT_TRUE(entries.empty());

    // Response without href is dropped
    EXPECT_TRUE(parse_dav_propfind_response(
        "<d:multistatus xmlns:d=\"DAV:\"><d:response>"
        "<d:propstat><d:prop><d:displayname>nope</d:displayname></d:prop>"
        "</d:propstat></d:response></d:multistatus>",
        &entries));
    EXPECT_TRUE(entries.empty());

    // Namespace prefixes, collection detection, fields, and quota
    const std::string xml =
        "<D:multistatus xmlns:D=\"DAV:\">"
        "<D:response>"
        "<D:href>/dav/docs/</D:href>"
        "<D:propstat><D:prop>"
        "<D:resourcetype><D:collection/></D:resourcetype>"
        "<D:displayname>docs</D:displayname>"
        "<D:getlastmodified>Mon, 02 Feb 2026 03:04:05 GMT</D:getlastmodified>"
        "<D:getetag>&quot;abc&quot;</D:getetag>"
        "<D:getcontenttype>httpd/unix-directory</D:getcontenttype>"
        "</D:prop></D:propstat>"
        "</D:response>"
        "<D:response>"
        "<D:href>https://host/dav/a%20b.txt</D:href>"
        "<D:propstat><D:prop>"
        "<D:resourcetype/>"
        "<D:getcontentlength>123</D:getcontentlength>"
        "<D:getcontenttype>text/plain</D:getcontenttype>"
        "</D:prop></D:propstat>"
        "</D:response>"
        "</D:multistatus>";
    EXPECT_TRUE(parse_dav_propfind_response(xml, &entries));
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_TRUE(entries[0].is_dir);
    EXPECT_EQ(entries[0].path, "/dav/docs/");
    EXPECT_EQ(entries[0].name, "docs");
    EXPECT_EQ(entries[0].etag, "\"abc\"");
    EXPECT_EQ(entries[0].content_type, "httpd/unix-directory");
    EXPECT_FALSE(entries[1].is_dir);
    EXPECT_EQ(entries[1].path, "/dav/a b.txt");
    EXPECT_EQ(entries[1].size, 123u);
    EXPECT_EQ(entries[1].content_type, "text/plain");

    // Non-numeric length and huge length fall back to 0
    entries.clear();
    const std::string bad =
        "<d:multistatus xmlns:d=\"DAV:\"><d:response>"
        "<d:href>/x</d:href><d:propstat><d:prop>"
        "<d:getcontentlength>not-a-number</d:getcontentlength>"
        "</d:prop></d:propstat></d:response></d:multistatus>";
    EXPECT_TRUE(parse_dav_propfind_response(bad, &entries));
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].size, 0u);
    entries.clear();
    const std::string huge =
        "<d:multistatus xmlns:d=\"DAV:\"><d:response>"
        "<d:href>/x</d:href><d:propstat><d:prop>"
        "<d:getcontentlength>999999999999999999999</d:getcontentlength>"
        "</d:prop></d:propstat></d:response></d:multistatus>";
    EXPECT_TRUE(parse_dav_propfind_response(huge, &entries));
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].size, 0u);
}

TEST(XmlScanCoverageTest, DavQuotaMatrix) {
    uint64_t used = 0;
    uint64_t avail = 0;

    const std::string ok =
        "<d:prop><d:quota-used-bytes>123</d:quota-used-bytes>"
        "<d:quota-available-bytes>456</d:quota-available-bytes></d:prop>";
    EXPECT_TRUE(parse_dav_quota(ok, &used, &avail));
    EXPECT_EQ(used, 123u);
    EXPECT_EQ(avail, 456u);

    // Missing both
    used = avail = 77;
    EXPECT_FALSE(parse_dav_quota("<d:prop></d:prop>", &used, &avail));
    EXPECT_EQ(used, 77u);
    EXPECT_EQ(avail, 77u);

    // Invalid numbers keep previous values
    used = avail = 99;
    const std::string bad =
        "<d:prop><d:quota-used-bytes>bad</d:quota-used-bytes>"
        "<d:quota-available-bytes>also-bad</d:quota-available-bytes></d:prop>";
    EXPECT_FALSE(parse_dav_quota(bad, &used, &avail));
    EXPECT_EQ(used, 99u);
    EXPECT_EQ(avail, 99u);

    // Overflow numbers are ignored
    used = avail = 5;
    const std::string over =
        "<d:prop><d:quota-used-bytes>999999999999999999999</d:quota-used-bytes>"
        "</d:prop>";
    EXPECT_FALSE(parse_dav_quota(over, &used, &avail));
    EXPECT_EQ(used, 5u);
}

#include "s3_list_xml.hpp"

using falcon::detail::S3ListEntry;
using falcon::detail::parse_s3_list_bucket_result;

TEST(XmlScanCoverageTest, S3ListBucketResultMatrix) {
    std::vector<S3ListEntry> out;

    // Not a ListBucketResult => false
    EXPECT_FALSE(parse_s3_list_bucket_result("<xml></xml>", &out));
    EXPECT_FALSE(parse_s3_list_bucket_result("", &out));

    // Empty result => true, zero entries
    EXPECT_TRUE(parse_s3_list_bucket_result(
        "<ListBucketResult></ListBucketResult>", &out));
    EXPECT_TRUE(out.empty());

    // Contents + CommonPrefixes, including missing fields, bad size,
    // trailing-slash prefix normalization, and key-only entries.
    const std::string xml =
        "<ListBucketResult>"
        "<Contents><Key>a.txt</Key><Size>10</Size>"
        "<LastModified>2026-01-01T00:00:00Z</LastModified>"
        "<ETag>\"etag1\"</ETag><StorageClass>STANDARD</StorageClass></Contents>"
        "<Contents><Key>b.bin</Key><Size>bad</Size></Contents>"
        "<Contents><Key>empty.txt</Key></Contents>"
        "<CommonPrefixes><Prefix>docs/</Prefix></CommonPrefixes>"
        "<CommonPrefixes><Prefix>img</Prefix></CommonPrefixes>"
        "<CommonPrefixes><Prefix>/</Prefix></CommonPrefixes>"
        "</ListBucketResult>";
    EXPECT_TRUE(parse_s3_list_bucket_result(xml, &out));
    ASSERT_EQ(out.size(), 5u);
    EXPECT_EQ(out[0].key, "a.txt");
    EXPECT_EQ(out[0].size, 10u);
    EXPECT_EQ(out[0].last_modified, "2026-01-01T00:00:00Z");
    EXPECT_EQ(out[0].etag, "\"etag1\"");
    EXPECT_EQ(out[0].storage_class, "STANDARD");
    EXPECT_FALSE(out[0].is_prefix);
    EXPECT_EQ(out[1].key, "b.bin");
    EXPECT_EQ(out[1].size, 0u);
    EXPECT_EQ(out[2].key, "empty.txt");
    EXPECT_EQ(out[2].size, 0u);
    EXPECT_TRUE(out[3].is_prefix);
    EXPECT_EQ(out[3].key, "docs");
    EXPECT_TRUE(out[4].is_prefix);
    EXPECT_EQ(out[4].key, "img");
}
