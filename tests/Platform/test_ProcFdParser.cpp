/// @file test_ProcFdParser.cpp
/// @brief Platform::ProcFd (#183): /proc/[pid]/fd link targets typed (files, deleted files, paths with
/// spaces, directories, devices, sockets, pipes, anon inodes, memfds, namespaces), fdinfo flags parsed,
/// and fd entry names. Pure text: runs on every platform.

#include "Platform/IProcessOpenFiles.h"
#include "Platform/Linux/ProcFdParser.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>

namespace Platform::ProcFd
{
namespace
{

TEST(ProcFdParserTest, ClassifiesFilesAndKeepsSpaces)
{
    const OpenFile file = classifyFdLink(4, "/home/me/My Documents/notes 1.txt", std::nullopt);
    EXPECT_EQ(file.descriptor, 4U);
    EXPECT_EQ(file.kind, OpenFileKind::File);
    EXPECT_EQ(file.path, "/home/me/My Documents/notes 1.txt");
    EXPECT_FALSE(file.deleted);
}

TEST(ProcFdParserTest, DeletedFileLosesTheSuffixAndIsMarked)
{
    const OpenFile file = classifyFdLink(5, "/tmp/scratch file (deleted)", std::nullopt);
    EXPECT_EQ(file.kind, OpenFileKind::File);
    EXPECT_EQ(file.path, "/tmp/scratch file");
    EXPECT_TRUE(file.deleted);
}

TEST(ProcFdParserTest, KernelObjectsKeepTheirNames)
{
    EXPECT_EQ(classifyFdLink(3, "socket:[12345]", std::nullopt).kind, OpenFileKind::Socket);
    EXPECT_EQ(classifyFdLink(3, "pipe:[678]", std::nullopt).kind, OpenFileKind::Pipe);
    EXPECT_EQ(classifyFdLink(3, "anon_inode:[eventfd]", std::nullopt).kind, OpenFileKind::AnonInode);
    EXPECT_EQ(classifyFdLink(3, "anon_inode:inotify", std::nullopt).kind, OpenFileKind::AnonInode);
    const OpenFile ns = classifyFdLink(3, "net:[4026531840]", std::nullopt);
    EXPECT_EQ(ns.kind, OpenFileKind::Other);
    EXPECT_EQ(ns.path, "net:[4026531840]");
    EXPECT_EQ(classifyFdLink(3, "socket:[12345]", std::nullopt).path, "socket:[12345]");
}

TEST(ProcFdParserTest, MemfdIsNotADeletedFile)
{
    const OpenFile memfd = classifyFdLink(7, "/memfd:wayland-shm (deleted)", std::nullopt);
    EXPECT_EQ(memfd.kind, OpenFileKind::Memfd);
    EXPECT_EQ(memfd.path, "memfd:wayland-shm");
    EXPECT_FALSE(memfd.deleted);
}

TEST(ProcFdParserTest, DevicesAndDirectories)
{
    EXPECT_EQ(classifyFdLink(0, "/dev/pts/0", std::nullopt).kind, OpenFileKind::Device);
    EXPECT_EQ(classifyFdLink(0, "/dev/null", std::nullopt).kind, OpenFileKind::Device);
    EXPECT_EQ(classifyFdLink(0, "/dev/shm/pulse-shm-1", std::nullopt).kind, OpenFileKind::File);
    EXPECT_EQ(classifyFdLink(0, "/home/me", DIRECTORY).kind, OpenFileKind::Directory);
    EXPECT_EQ(classifyFdLink(0, "/home/me", std::optional<std::uint32_t>(02)).kind, OpenFileKind::File);
}

TEST(ProcFdParserTest, ParsesFdInfoFlags)
{
    EXPECT_EQ(parseFdInfoFlags("pos:\t0\nflags:\t02100002\nmnt_id:\t25\n"), std::optional<std::uint32_t>(02100002));
    EXPECT_EQ(parseFdInfoFlags("flags:\t0100000"), std::optional<std::uint32_t>(0100000)); // no trailing newline
    EXPECT_EQ(parseFdInfoFlags("pos:\t0\n"), std::nullopt);
    EXPECT_EQ(parseFdInfoFlags("flags:\t\n"), std::nullopt);
    EXPECT_EQ(parseFdInfoFlags("flags:\tzz\n"), std::nullopt);
    EXPECT_EQ(parseFdInfoFlags(""), std::nullopt);
}

TEST(ProcFdParserTest, ParsesFdNames)
{
    EXPECT_EQ(parseFdName("0"), std::optional<std::uint64_t>(0));
    EXPECT_EQ(parseFdName("1023"), std::optional<std::uint64_t>(1023));
    EXPECT_EQ(parseFdName("."), std::nullopt);
    EXPECT_EQ(parseFdName(".."), std::nullopt);
    EXPECT_EQ(parseFdName(""), std::nullopt);
    EXPECT_EQ(parseFdName("12x"), std::nullopt);
}

} // namespace
} // namespace Platform::ProcFd
