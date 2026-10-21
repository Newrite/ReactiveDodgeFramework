#include "check.h"

#include "core/IniWriter.h"

#include <vector>

using rdf::ini::Entry;
using rdf::ini::Update;

TEST(ini_rewrites_a_key_in_place_and_keeps_comments_and_crlf)
{
	const std::string text = "; head\r\n[Motion]\r\n; how far\r\nfDistance=300\r\nfRunSpeed=300\r\n\r\n[Steering]\r\nfMaxDeviation=180\r\n";
	const std::vector<Entry> e{ { "Motion", "fDistance", "420" } };
	CHECK(Update(text, e) == "; head\r\n[Motion]\r\n; how far\r\nfDistance=420\r\nfRunSpeed=300\r\n\r\n[Steering]\r\nfMaxDeviation=180\r\n");
}

TEST(ini_matches_section_and_key_case_insensitively_and_skips_commented_keys)
{
	const std::string text = "[steering]\n;fMaxDeviation=5\n FMAXDEVIATION = 180\n";
	const std::vector<Entry> e{ { "Steering", "fMaxDeviation", "0" } };
	CHECK(Update(text, e) == "[steering]\n;fMaxDeviation=5\nfMaxDeviation=0\n");
}

TEST(ini_same_key_in_another_section_is_left_alone)
{
	const std::string text = "[A]\nx=1\n[B]\nx=2\n";
	const std::vector<Entry> e{ { "B", "x", "9" } };
	CHECK(Update(text, e) == "[A]\nx=1\n[B]\nx=9\n");
}

TEST(ini_missing_key_goes_after_the_last_line_of_its_section)
{
	const std::string text = "[Graph]\nfClipLength=0.667\n\n[Dev]\nbDevLog=0\n";
	const std::vector<Entry> e{ { "Graph", "fArmStanceFirstPerson", "1" } };
	CHECK(Update(text, e) == "[Graph]\nfClipLength=0.667\nfArmStanceFirstPerson=1\n\n[Dev]\nbDevLog=0\n");
}

TEST(ini_missing_section_is_appended)
{
	const std::string text = "[Dev]\r\nbDevLog=0\r\n";
	const std::vector<Entry> e{ { "Jump", "bBlockDuringDodge", "1" } };
	CHECK(Update(text, e) == "[Dev]\r\nbDevLog=0\r\n\r\n[Jump]\r\nbBlockDuringDodge=1\r\n");
}

TEST(ini_without_a_final_newline_stays_that_way_and_empty_file_works)
{
	const std::vector<Entry> e{ { "Dev", "bDevLog", "1" } };
	CHECK(Update("[Dev]\nbDevLog=0", e) == "[Dev]\nbDevLog=1");
	CHECK(Update("", e) == "[Dev]\r\nbDevLog=1\r\n");
}

TEST(ini_writing_the_same_values_twice_changes_nothing)
{
	const std::string text = "[Motion]\r\nfDistance=300\r\n";
	const std::vector<Entry> e{ { "Motion", "fDistance", "300" }, { "Motion", "fRunSpeed", "300" } };
	const auto once = Update(text, e);
	CHECK(Update(once, e) == once);
}
