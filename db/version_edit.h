// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_VERSION_EDIT_H_
#define STORAGE_LEVELDB_DB_VERSION_EDIT_H_

#include <set>
#include <utility>
#include <vector>
#include "db/dbformat.h"

namespace leveldb {

class VersionSet;

struct FileMetaData {
  int refs;
  int allowed_seeks;          // Seeks allowed until compaction
  uint64_t number;
  uint64_t file_size;         // File size in bytes
  InternalKey smallest;       // Smallest internal key served by table
  InternalKey largest;        // Largest internal key served by table

  FileMetaData() : refs(0), allowed_seeks(1 << 30), file_size(0) { }
};

class VersionEdit { //VersionEdit���൱��MANIFEST�ļ��е�һ����¼��
 public:
  VersionEdit() { Clear(); }
  ~VersionEdit() { }

  void Clear();

  void SetComparatorName(const Slice& name) {
    has_comparator_ = true;
    comparator_ = name.ToString();
  }
  void SetLogNumber(uint64_t num) {
    has_log_number_ = true;
    log_number_ = num;
  }
  void SetPrevLogNumber(uint64_t num) {
    has_prev_log_number_ = true;
    prev_log_number_ = num;
  }
  void SetNextFile(uint64_t num) {
    has_next_file_number_ = true;
    next_file_number_ = num;
  }
  void SetLastSequence(SequenceNumber seq) {
    has_last_sequence_ = true;
    last_sequence_ = seq;
  }
  void SetCompactPointer(int level, const InternalKey& key) {
    compact_pointers_.push_back(std::make_pair(level, key));
  }

  // definition and implemention
  // S1������ӵ�sstable���ļ�number����С��smallestkey��largest��һ��ֵ��FileMetaData
  // S2����level��FileMetaData��Ϣ��Ϊһ��pair��ӵ�VersionEdit��new_files_��
  // std::vector< std::pair<int, FileMetaData> > new_files_;
  // REQUIRES: This version has not been saved (see VersionSet::SaveTo)
  // REQUIRES: "smallest" and "largest" are smallest and largest keys in file
  void AddFile(int level, uint64_t file,
               uint64_t file_size,
               const InternalKey& smallest,
               const InternalKey& largest) {
	// S1������ӵ�sstable���ļ�number����С��smallestkey��largest��һ��ֵ��FileMetaData
    FileMetaData f;
    f.number = file;
    f.file_size = file_size;
    f.smallest = smallest;
    f.largest = largest;
	// S2����level��FileMetaData��Ϣ��Ϊһ��pair��ӵ�VersionEdit��new_files_��
    new_files_.push_back(std::make_pair(level, f));
  }

  //��� ��ָ����levelɾ��ָ��number��sst  ����ϢԤ��¼�� Delete the specified "file" from the specified "level".
  void DeleteFile(int level, uint64_t file) {
    deleted_files_.insert(std::make_pair(level, file));
  }

  // ��VersionEdit(this)����ϢEncode��һ��string�У���Ϊһ��record������֮��д��manifest�ļ� 
  void EncodeTo(std::string* dst) const; 
  // ��Slice��Decode��VersionEdit����Ϣ  
  Status DecodeFrom(const Slice& src);

  std::string DebugString() const;

 private:
  friend class VersionSet;

  typedef std::set< std::pair<int, uint64_t> > DeletedFileSet;

  std::string comparator_; // key comparator����  
  uint64_t log_number_; // ��ǰversionEdit������bin��log���**   
  uint64_t prev_log_number_; // ǰһ����־���,Ϊ�˼����ϰ汾���°汾�����ã�һֱΪ0  
  uint64_t next_file_number_; // ��һ���ļ����  ָ����manifest�ļ���ӡ����manifestֻ����һ��
  SequenceNumber last_sequence_; // db������seq�����кţ��������һ��kv������������к�
  //Manifest�ļ������ȴ洢����coparator����log��š�ǰһ��log��š���һ���ļ���š���һ�����кš�
  //��Щ������־��sstable�ļ�ʹ�õ�����Ҫ��Ϣ����Щ�ֶβ�һ����Ȼ���ڡ�
  bool has_comparator_;
  bool has_log_number_;
  bool has_prev_log_number_;
  bool has_next_file_number_;
  bool has_last_sequence_;

  std::vector< std::pair<int, InternalKey> > compact_pointers_; //��¼ÿһ���´κϲ�����ʼkey
  DeletedFileSet deleted_files_; //�ڴ˴�VersionEdit��delta����ɾ�����ļ����ϡ���saveʱʵ��ִ����Ӳ���**
  std::vector< std::pair<int, FileMetaData> > new_files_; //�ڴ˴�VersionEdit��delta��������ӵ�sst���ϡ� 
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_VERSION_EDIT_H_
