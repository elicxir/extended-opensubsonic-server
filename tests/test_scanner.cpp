#include <gtest/gtest.h>

#include <fstream>
#include <memory>
#include <tuple>

#include "core/config.h"
#include "db/migrate.h"
#include "db/write_queue.h"
#include "fixtures.h"
#include "scanner/scanner.h"
#include "scanner/tag_report.h"

using namespace eos;
namespace fs = std::filesystem;

namespace {

class ScannerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!test::have_flac_encoder()) GTEST_SKIP() << "flac encoder not installed";
    music = dir.path() / "music";
    fs::create_directories(music);
    cfg.data_dir = dir.path() / "data";
    cfg.libraries = {{"Music", music}};
    cfg.tags = default_custom_tags();
    fs::create_directories(cfg.data_dir);
    {
      db::Connection c(cfg.db_path());
      db::migrate(c);
    }
    writer = std::make_unique<db::WriteQueue>(cfg.db_path());
    scanner = std::make_unique<scanner::Scanner>(cfg, *writer, cfg.db_path());
    conn = std::make_unique<db::Connection>(cfg.db_path());
  }

  void make_basic_library() {
    test::make_flac(music / "A/01.flac", 440,
                    {{"TITLE", {"Opening"}}, {"ALBUM", {"Album A"}}, {"ARTIST", {"Alice"}},
                     {"ALBUMARTIST", {"Alice"}}, {"COMPOSER", {"Carol"}}, {"TRACKNUMBER", {"1"}},
                     {"DISCNUMBER", {"1"}}, {"DISCSUBTITLE", {"Disc One"}}, {"GENRE", {"Soundtrack"}},
                     {"PERFORMER", {"Dave (piano)"}}, {"DATE", {"2020-04-01"}}});
    test::make_flac(music / "A/02.flac", 550,
                    {{"TITLE", {"Battle"}}, {"ALBUM", {"Album A"}}, {"ARTIST", {"Alice; Bob"}},
                     {"ALBUMARTIST", {"Alice"}}, {"TRACKNUMBER", {"2"}}, {"DISCNUMBER", {"1"}},
                     {"DATE", {"2020-04-01"}}});
    test::make_flac(music / "B/01.flac", 660,
                    {{"TITLE", {"Theme"}}, {"ALBUM", {"Album B"}}, {"ARTIST", {"Erin"}},
                     {"TRACKNUMBER", {"1"}}, {"COLLECTION_WORK", {"Game X; Game Y"}}});
  }

  std::string id_at(const std::string& rel) {
    return conn->scalar_text("SELECT id FROM track WHERE path = ?", rel).value_or("");
  }

  test::TempDir dir;
  fs::path music;
  Config cfg;
  std::unique_ptr<db::WriteQueue> writer;
  std::unique_ptr<scanner::Scanner> scanner;
  std::unique_ptr<db::Connection> conn;
};

}  // namespace

TEST_F(ScannerTest, InitialScanBuildsTracksAlbumsAndPeople) {
  make_basic_library();
  auto s = scanner->scan(false);
  EXPECT_TRUE(s.errors.empty());
  EXPECT_EQ(s.seen, 3);
  EXPECT_EQ(s.added, 3);
  EXPECT_EQ(conn->scalar_int("SELECT COUNT(*) FROM track"), 3);
  EXPECT_EQ(conn->scalar_int("SELECT COUNT(*) FROM album"), 2);

  // A new track's id is its PID.
  EXPECT_EQ(conn->scalar_int("SELECT COUNT(*) FROM track WHERE id = pid"), 3);
  auto album_a = conn->scalar_text("SELECT id FROM album WHERE name = 'Album A'");
  ASSERT_TRUE(album_a);
  EXPECT_EQ(conn->scalar_int("SELECT song_count FROM album WHERE id = ?", *album_a), 2);
  EXPECT_EQ(conn->scalar_text("SELECT title FROM album_disc WHERE album_id = ?", *album_a), "Disc One");
  EXPECT_EQ(conn->scalar_text("SELECT kind FROM album WHERE id = ?", *album_a), "physical");

  auto opening = id_at("A/01.flac");
  EXPECT_TRUE(conn->scalar_text("SELECT audio_md5 FROM track WHERE id = ?", opening));
  EXPECT_EQ(conn->scalar_text("SELECT display_composer FROM track WHERE id = ?", opening), "Carol");
  EXPECT_EQ(conn->scalar_text("SELECT a.name FROM participant p JOIN artist a ON a.id = p.artist_id "
                              "WHERE p.entity_id = ? AND p.role = 'performer'", opening),
            "Dave");
  EXPECT_EQ(conn->scalar_text("SELECT sub_role FROM participant WHERE entity_id = ? AND role = "
                              "'performer'", opening),
            "piano");
  // "Alice; Bob" splits into two artists.
  EXPECT_EQ(conn->scalar_int("SELECT COUNT(*) FROM participant WHERE entity_id = ? AND role = 'artist'",
                             id_at("A/02.flac")),
            2);
  EXPECT_EQ(conn->scalar_int("SELECT year FROM track WHERE id = ?", opening), 2020);
}

TEST_F(ScannerTest, SecondScanSkipsUnchangedFiles) {
  make_basic_library();
  scanner->scan(false);
  auto s = scanner->scan(false);
  EXPECT_EQ(s.unchanged, 3);
  EXPECT_EQ(s.added + s.updated + s.moved, 0);
  auto full = scanner->scan(true);
  EXPECT_EQ(full.updated, 3);
  EXPECT_EQ(conn->scalar_int("SELECT COUNT(*) FROM track"), 3);
}

TEST_F(ScannerTest, RenameKeepsIdAndManualLinks) {
  make_basic_library();
  scanner->scan(false);
  auto id = id_at("B/01.flac");
  conn->run("INSERT INTO title (id, name, name_norm, source, created_at, updated_at) "
            "VALUES ('t1', 'Manual Title', 'manualtitle', 3, 0, 0)");
  conn->run("INSERT INTO track_title (track_id, title_id, source) VALUES (?, 't1', 3)", id);

  fs::create_directories(music / "C");
  fs::rename(music / "B/01.flac", music / "C/renamed.flac");
  auto s = scanner->scan(false);
  EXPECT_EQ(s.moved, 1);
  EXPECT_EQ(s.added, 0);
  EXPECT_EQ(s.missing, 0);
  EXPECT_EQ(id_at("C/renamed.flac"), id);
  EXPECT_EQ(conn->scalar_int("SELECT COUNT(*) FROM track_title WHERE track_id = ? AND source = 3", id), 1);
}

TEST_F(ScannerTest, RetagThenMoveIsMatchedByAudioMd5) {
  make_basic_library();
  scanner->scan(false);
  auto id = id_at("A/01.flac");
  auto old_pid = conn->scalar_text("SELECT pid FROM track WHERE id = ?", id);
  test::retag_flac(music / "A/01.flac", {{"TITLE", {"Opening (Remaster)"}}, {"ALBUM", {"Album A"}},
                                         {"ALBUMARTIST", {"Alice"}}, {"TRACKNUMBER", {"1"}},
                                         {"DISCNUMBER", {"1"}}, {"DATE", {"2020-04-01"}}});
  fs::rename(music / "A/01.flac", music / "A/01-remaster.flac");
  auto s = scanner->scan(false);
  EXPECT_EQ(s.moved, 1);
  EXPECT_EQ(s.added, 0);
  EXPECT_EQ(id_at("A/01-remaster.flac"), id);
  EXPECT_EQ(conn->scalar_text("SELECT title FROM track WHERE id = ?", id), "Opening (Remaster)");
  EXPECT_NE(conn->scalar_text("SELECT pid FROM track WHERE id = ?", id), old_pid);
}

TEST_F(ScannerTest, DeletedFileGoesMissingAndComesBackWithSameId) {
  make_basic_library();
  scanner->scan(false);
  auto id = id_at("A/02.flac");
  auto album = conn->scalar_text("SELECT album_id FROM track WHERE id = ?", id);
  auto stash = dir.path() / "stash.flac";
  fs::rename(music / "A/02.flac", stash);

  auto s1 = scanner->scan(false);
  EXPECT_EQ(s1.missing, 1);
  EXPECT_EQ(conn->scalar_int("SELECT missing FROM track WHERE id = ?", id), 1);
  EXPECT_EQ(conn->scalar_int("SELECT song_count FROM album WHERE id = ?", album), 1);

  fs::rename(stash, music / "A/02-back.flac");
  auto s2 = scanner->scan(false);
  EXPECT_EQ(s2.moved, 1);
  EXPECT_EQ(id_at("A/02-back.flac"), id);
  EXPECT_EQ(conn->scalar_int("SELECT missing FROM track WHERE id = ?", id), 0);
  EXPECT_EQ(conn->scalar_int("SELECT song_count FROM album WHERE id = ?", album), 2);
}

TEST_F(ScannerTest, RenamedAlbumKeepsItsId) {
  make_basic_library();
  scanner->scan(false);
  auto album = conn->scalar_text("SELECT id FROM album WHERE name = 'Album A'");
  for (auto [file, title, n] : {std::tuple{"A/01.flac", "Opening", "1"}, {"A/02.flac", "Battle", "2"}})
    test::retag_flac(music / file, {{"TITLE", {title}}, {"ALBUM", {"Album A (Deluxe)"}},
                                    {"ALBUMARTIST", {"Alice"}}, {"TRACKNUMBER", {n}},
                                    {"DATE", {"2020-04-01"}}});
  scanner->scan(false);
  EXPECT_EQ(conn->scalar_text("SELECT id FROM album WHERE name = 'Album A (Deluxe)'"), album);
  EXPECT_EQ(conn->scalar_int("SELECT COUNT(*) FROM album"), 2);
}

TEST_F(ScannerTest, OnlyReleaseDateSplitsAnAlbum) {
  // Recording years in DATE (a compilation of live recordings) keep one album...
  test::make_flac(music / "H/01.flac", 700, {{"TITLE", {"a"}}, {"ALBUM", {"Art of H"}}, {"ALBUMARTIST", {"H"}},
                                             {"TRACKNUMBER", {"1"}}, {"DATE", {"1962"}}});
  test::make_flac(music / "H/02.flac", 710, {{"TITLE", {"b"}}, {"ALBUM", {"Art of H"}}, {"ALBUMARTIST", {"H"}},
                                             {"TRACKNUMBER", {"2"}}, {"DATE", {"1971"}}});
  // ...while different release dates (a reissue) make two.
  test::make_flac(music / "R/01.flac", 720, {{"TITLE", {"c"}}, {"ALBUM", {"Reissued"}}, {"ALBUMARTIST", {"R"}},
                                             {"RELEASEDATE", {"1990"}}});
  test::make_flac(music / "R2/01.flac", 730, {{"TITLE", {"c"}}, {"ALBUM", {"Reissued"}}, {"ALBUMARTIST", {"R"}},
                                              {"RELEASEDATE", {"2020"}}});
  scanner->scan(false);
  EXPECT_EQ(conn->scalar_int("SELECT COUNT(*) FROM album WHERE name = 'Art of H'"), 1);
  EXPECT_EQ(conn->scalar_int("SELECT song_count FROM album WHERE name = 'Art of H'"), 2);
  EXPECT_EQ(conn->scalar_int("SELECT COUNT(*) FROM album WHERE name = 'Reissued'"), 2);
}

TEST_F(ScannerTest, ManualOverrideWinsOverTags) {
  make_basic_library();
  scanner->scan(false);
  auto id = id_at("B/01.flac");
  conn->run("INSERT INTO field_override (entity_type, entity_id, field, value, updated_at) "
            "VALUES ('track', ?, 'title', 'Main Theme', 0)", id);
  test::retag_flac(music / "B/01.flac", {{"TITLE", {"Theme v2"}}, {"ALBUM", {"Album B"}},
                                         {"ARTIST", {"Erin"}}, {"TRACKNUMBER", {"1"}}});
  scanner->scan(false);
  EXPECT_EQ(conn->scalar_text("SELECT title FROM track WHERE id = ?", id), "Main Theme");
}

TEST_F(ScannerTest, MissingTracksArePurgedAfterGracePeriod) {
  cfg.purge_missing_after_days = 0;
  make_basic_library();
  scanner->scan(false);
  fs::remove(music / "B/01.flac");
  scanner->scan(false);  // marks missing
  EXPECT_EQ(conn->scalar_int("SELECT COUNT(*) FROM track WHERE missing = 1"), 1);
  scanner->scan(false);  // purges
  EXPECT_EQ(conn->scalar_int("SELECT COUNT(*) FROM track"), 2);
  EXPECT_EQ(conn->scalar_int("SELECT COUNT(*) FROM album"), 1);
  EXPECT_FALSE(conn->scalar_int("SELECT 1 FROM artist WHERE name = 'Erin'"));
}

TEST_F(ScannerTest, SkipsIgnoredHiddenAndOtherFiles) {
  make_basic_library();
  test::make_flac(music / "skip/x.flac", 700, {{"TITLE", {"Ignored"}}});
  std::ofstream(music / "skip/.ndignore").put('\n');
  test::make_flac(music / ".hidden/y.flac", 710, {{"TITLE", {"Hidden"}}});
  std::ofstream(music / "A/cover.jpg") << "not audio";
  auto s = scanner->scan(false);
  EXPECT_EQ(s.seen, 3);
}

TEST_F(ScannerTest, UnreadableFileIsReportedNotFatal) {
  make_basic_library();
  std::ofstream(music / "A/broken.flac") << "garbage";
  auto s = scanner->scan(false);
  EXPECT_EQ(s.added, 3);
  ASSERT_EQ(s.errors.size(), 1u);
  EXPECT_NE(s.errors[0].find("broken.flac"), std::string::npos);
}

TEST_F(ScannerTest, TagReportCountsKeyTags) {
  make_basic_library();
  scanner->scan(false);
  auto r = scanner::build_tag_report(*conn);
  EXPECT_EQ(r.tracks, 3);
  EXPECT_EQ(r.albums, 2);
  auto find = [&](const std::string& tag) {
    for (const auto& row : r.key_tags)
      if (row.tag == tag) return row.tracks;
    return int64_t{-1};
  };
  EXPECT_EQ(find("COMPOSER"), 1);
  EXPECT_EQ(find("COLLECTION_WORK"), 1);
  EXPECT_EQ(find("MUSICBRAINZ_TRACKID"), 0);
}
