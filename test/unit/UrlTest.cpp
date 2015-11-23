#include "gtest/gtest.h"

#include "Url.h"

TEST(UrlTest, SetNonAsciiValid) {
	char* input_urls[] = {
	    "http://topbeskæring.dk/velkommen",
	    "www.Alliancefrançaise.nu",
	    "française.Alliance.nu",
	    "française.Alliance.nu/asdf",
	    "http://française.Alliance.nu/asdf",
	    "http://française.Alliance.nu/",
	    "幸运.龍.com",
	    "幸运.龍.com/asdf/运/abc",
	    "幸运.龍.com/asdf",
	    "http://幸运.龍.com/asdf",
	    "http://Беларуская.org/Акадэмічная",
	    "https://hi.Български.com",
	    "https://fakedomain.中文.org/asdf",
	    "https://gigablast.com/abc/文/efg",
	    "https://gigablast.com/?q=文",
	    "http://www.example.сайт",
	    "http://genocidearchiverwanda.org.rw/index.php/Category:Official_Communiqués",
	    "http://www.example.com/xn--fooled-you-into-trying-to-decode-this",
	    "http://www.example.сайт/xn--fooled-you-into-trying-to-decode-this",
	    "http://腕時計通販.jp/"
	};

	const char* expected_normalized[] = {
	    "http://xn--topbeskring-g9a.dk/velkommen",
	    "http://www.xn--alliancefranaise-npb.nu/",
	    "http://xn--franaise-v0a.alliance.nu/",
	    "http://xn--franaise-v0a.alliance.nu/asdf",
	    "http://xn--franaise-v0a.alliance.nu/asdf",
	    "http://xn--franaise-v0a.alliance.nu/",
	    "http://xn--lwt711i.xn--mi7a.com/",
	    "http://xn--lwt711i.xn--mi7a.com/asdf/%E8%BF%90/abc",
	    "http://xn--lwt711i.xn--mi7a.com/asdf",
	    "http://xn--lwt711i.xn--mi7a.com/asdf",
	    "http://xn--d0a6das0ae0bir7j.org/%D0%90%D0%BA%D0%B0%D0%B4%D1%8D%D0%BC%D1%96%D1%87%D0%BD%D0%B0%D1%8F",
	    "https://hi.xn--d0a6divjd1bi0f.com/",
	    "https://fakedomain.xn--fiq228c.org/asdf",
	    "https://gigablast.com/abc/%E6%96%87/efg",
	    "https://gigablast.com/?q=%E6%96%87",
	    "http://www.example.xn--80aswg/",
	    "http://genocidearchiverwanda.org.rw/index.php/Category:Official_Communiqu%C3%A9s",
	    "http://www.example.com/xn--fooled-you-into-trying-to-decode-this",
	    "http://www.example.xn--80aswg/xn--fooled-you-into-trying-to-decode-this",
	    "http://xn--kjvp61d69f6wc3zf.jp/"
	};

	ASSERT_EQ(sizeof(input_urls), sizeof(expected_normalized));

	size_t len = sizeof(input_urls) / sizeof(input_urls[0]);
	for (size_t i = 0; i < len; i++) {
		Url url;
		url.set(input_urls[i], strlen(input_urls[i]));

		EXPECT_STREQ(expected_normalized[i], (const char*)url.getUrl());
	}
}

TEST(UrlTest, SetNonAsciiInValid) {
	char* input_urls[] = {
	    "http://www.fas.org/blog/ssp/2009/08/securing-venezuela\032s-arsenals.php",
	    "https://pypi.python\n\n\t\t\t\t.org/packages/source/p/pyramid/pyramid-1.5.tar.gz",
	    "http://undocs.org/ru/A/C.3/68/\vSR.48"
	};

	const char* expected_normalized[] = {
	    "http://www.fas.org/blog/ssp/2009/08/securing-venezuela%1As-arsenals.php",
	    "https://pypi.python/",
		"http://undocs.org/ru/A/C.3/68/%0BSR.48"
	};

	ASSERT_EQ(sizeof(input_urls), sizeof(expected_normalized));

	size_t len = sizeof(input_urls) / sizeof(input_urls[0]);
	for (size_t i = 0; i < len; i++) {
		Url url;
		url.set(input_urls[i], strlen(input_urls[i]));

		EXPECT_STREQ(expected_normalized[i], (const char*)url.getUrl());
	}
}