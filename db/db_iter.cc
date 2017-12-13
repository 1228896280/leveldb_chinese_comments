// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/db_iter.h"

#include "db/filename.h"
#include "db/db_impl.h"
#include "db/dbformat.h"
#include "leveldb/env.h"
#include "leveldb/iterator.h"
#include "port/port.h"
#include "util/logging.h"
#include "util/mutexlock.h"
#include "util/random.h"

namespace leveldb {

#if 0
static void DumpInternalIter(Iterator* iter) {
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    ParsedInternalKey k;
    if (!ParseInternalKey(iter->key(), &k)) {
      fprintf(stderr, "Corrupt '%s'\n", EscapeString(iter->key()).c_str());
    } else {
      fprintf(stderr, "@ '%s'\n", k.DebugString().c_str());
    }
  }
}
#endif

namespace {

//Leveldb���ݿ��MemTable��sstable�ļ��Ĵ洢��ʽ����InternalKey(userkey, seq, type) = > uservalue��
//DBIter��ͬһ��userkey��DB�еĶ�����¼�ϲ�Ϊһ�����ۺϿ�����userkey����š�ɾ����ǡ���д���ǵȵ����ء�
//DBIterֻ���userkey���£�seq���ľ������µģ���ͬuserkey���ϼ�¼��seq��С�ģ��������ϲ㿴������һ����¼չ�ָ��û���
//��������������µļ�¼��ɾ�����ͣ���������ü�¼�����򣬱���ʱ�����ɾ����key�оٳ�����
class DBIter: public Iterator {
 public:
  // Which direction is the iterator currently moving?
  // (1) When moving forward, the internal iterator is positioned at
  //     the exact entry that yields this->key(), this->value()
  // (2) When moving backwards, the internal iterator is positioned
  //     just before all entries whose user key == this->key().
  enum Direction {
    kForward,
    kReverse
  };

  DBIter(DBImpl* db, const Comparator* cmp, Iterator* iter, SequenceNumber s,
         uint32_t seed)
      : db_(db),
        user_comparator_(cmp),
        iter_(iter),
        sequence_(s),
        direction_(kForward),
        valid_(false),
        rnd_(seed),
        bytes_counter_(RandomPeriod()) {
  }
  virtual ~DBIter() {
    delete iter_;
  }
  virtual bool Valid() const { return valid_; }
  virtual Slice key() const {
    assert(valid_);
    return (direction_ == kForward) ? ExtractUserKey(iter_->key()) : saved_key_;
  }
  virtual Slice value() const {
    assert(valid_);
    return (direction_ == kForward) ? iter_->value() : saved_value_;
  }
  virtual Status status() const {
    if (status_.ok()) {
      return iter_->status();
    } else {
      return status_;
    }
  }

  virtual void Next();
  virtual void Prev();
  virtual void Seek(const Slice& target);
  virtual void SeekToFirst();
  virtual void SeekToLast();

 private:
  void FindNextUserEntry(bool skipping, std::string* skip);
  void FindPrevUserEntry();
  bool ParseKey(ParsedInternalKey* key);

  inline void SaveKey(const Slice& k, std::string* dst) {
    dst->assign(k.data(), k.size());
  }

  inline void ClearSavedValue() {
    if (saved_value_.capacity() > 1048576) {
      std::string empty;
      swap(empty, saved_value_);
    } else {
      saved_value_.clear();
    }
  }

  // Pick next gap with average value of config::kReadBytesPeriod.
  ssize_t RandomPeriod() {
    return rnd_.Uniform(2*config::kReadBytesPeriod);
  }

  DBImpl* db_;
  const Comparator* const user_comparator_;//�Ƚ�iter��userkey
  Iterator* const iter_;//��һ��MergingIterator����DBImpl::NewInternalIterator()�����Ľ����
  //ͨ��MergingIterator����ʵ�ֶ���������ݼ��ϵĹ鲢���������а������child iterator��ɵļ��ϡ�
  SequenceNumber const sequence_;//DBIterֻ�ܷ��ʵ���sequence_С��kv��
  //��ͷ������ϰ汾�����գ����ݿ�ı���

  Status status_;
  std::string saved_key_;     // == current key when direction_==kReverse //���ڷ������ direction_==kReverseʱ����Ч
  std::string saved_value_;   // == current raw value when direction_==kReverse//���ڷ������ direction_==kReverseʱ����Ч
  Direction direction_;
  bool valid_;

  Random rnd_;
  ssize_t bytes_counter_;

  // No copying allowed
  DBIter(const DBIter&);
  void operator=(const DBIter&);
};

inline bool DBIter::ParseKey(ParsedInternalKey* ikey) {
  Slice k = iter_->key();
  ssize_t n = k.size() + iter_->value().size();
  bytes_counter_ -= n;
  while (bytes_counter_ < 0) {
    bytes_counter_ += RandomPeriod();
    db_->RecordReadSample(k);
  }
  if (!ParseInternalKey(k, ikey)) {
    status_ = Status::Corruption("corrupted internal key in DBIter");
    return false;
  } else {
    return true;
  }
}

void DBIter::Next() {
  assert(valid_);

  if (direction_ == kReverse) {  // Switch directions?
    direction_ = kForward;
    // iter_ is pointing just before the entries for this->key(),
    // so advance into the range of entries for this->key() and then
    // use the normal skipping code below.
    if (!iter_->Valid()) {
      iter_->SeekToFirst();
    } else {
      iter_->Next();
    }
    if (!iter_->Valid()) {
      valid_ = false;
      saved_key_.clear();
      return;
    }
    // saved_key_ already contains the key to skip past.
  } else {
    // Store in saved_key_ the current key so we skip it below.
    SaveKey(ExtractUserKey(iter_->key()), &saved_key_);
  }

  FindNextUserEntry(true, &saved_key_);
}


//FindNextUserEntry��FindPrevUserEntry�Ĺ��ܾ���ѭ��������һ�� / ǰһ��delete�ļ�¼��ֱ������kValueType�ļ�¼��
//FindNextUserKey�ƶ�������kForward��DBIter����kForward�ƶ�ʱ��������saved key��Ϊ��ʱ���档
//FindNextUserKeyȷ����λ����entry��sequence�������ָ����sequence����������ɾ����Ǹ��ǵľɼ�¼��
//����@skipping�����Ƿ�Ҫ����userkey��skip��ȵļ�¼��
//����@skip��ʱ�洢�ռ䣬����seekʱҪ������key��
//�ڽ���FindNextUserEntryʱ��iter_�պö�λ��this->key(), this->value()������¼�ϡ�
void DBIter::FindNextUserEntry(bool skipping, std::string* skip) {//skipping����true��ʱ���ʾҪ����ҵ���һ����skip����ȵ�userkey����
  // Loop until we hit an acceptable entry to yield
  assert(iter_->Valid());
  assert(direction_ == kForward);
  do {
    ParsedInternalKey ikey;
    if (ParseKey(&ikey) && ikey.sequence <= sequence_) {//����������sequence_Ҫ���kv��
      switch (ikey.type) {
        case kTypeDeletion://�������iter userkey��ɾ����������˵�������userkey������Ч�ģ������Ҫ����
          // Arrange to skip all upcoming entries for this key since
          // they are hidden by this deletion.
          SaveKey(ikey.user_key, skip);
          skipping = true;
          break;
        case kTypeValue:
          if (skipping &&
              user_comparator_->Compare(ikey.user_key, *skip) <= 0) {//�����ǰkey��Ҫ������key��Ⱦͼ������
            // Entry hidden
          } else {
            valid_ = true;//���skippingΪtrue�Ļ����˴�iterΪ��һ��������save��userkey,���Ҳ���ɾ������
            saved_key_.clear();
            return;
          }
          break;
      }
    }
    iter_->Next();
  } while (iter_->Valid());
  saved_key_.clear();
  valid_ = false;
}

void DBIter::Prev() {
  assert(valid_);

  if (direction_ == kForward) {  // Switch directions?
    // iter_ is pointing at the current entry.  Scan backwards until
    // the key changes so we can use the normal reverse scanning code.
    assert(iter_->Valid());  // Otherwise valid_ would have been false
    SaveKey(ExtractUserKey(iter_->key()), &saved_key_);
    while (true) {
      iter_->Prev();
      if (!iter_->Valid()) {
        valid_ = false;
        saved_key_.clear();
        ClearSavedValue();
        return;
      }
      if (user_comparator_->Compare(ExtractUserKey(iter_->key()),
                                    saved_key_) < 0) {
        break;
      }
    }
    direction_ = kReverse;
  }

  FindPrevUserEntry();
}

//�ڽ���FindPrevUserEntryʱ��iter_�պ�λ��saved key��Ӧ�����м�¼֮ǰ*FindPrevUserKey����ָ����sequence�����μ��ǰһ��entry��ֱ������user keyС��saved key���������Ͳ���Delete��entry��
//���entry��������Delete�������saved key��saved value�����������α���ǰһ��entry��ѭ���У�ֻҪ���Ͳ���Delete������Ҫ�ҵ�entry��
//�����Prev�����塣
void DBIter::FindPrevUserEntry() {
	assert(direction_ == kReverse);

	ValueType value_type = kTypeDeletion;//����ܹؼ��������ѭ������ִ��һ��Prev���� 

	if (iter_->Valid()) {
		do {
			ParsedInternalKey ikey;
			if (ParseKey(&ikey) && ikey.sequence <= sequence_) {
				if ((value_type != kTypeDeletion) &&//��һ���ǲ������÷�֧�ģ���Ϊ����ѭ��ǰ�Ȱ�value_type = kTypeDeletion;
					user_comparator_->Compare(ikey.user_key, saved_key_) < 0) {
					// We encountered a non-deleted value in entries for previous keys,
					break;
				}
				//�������ͣ������Deletion�����saved key��saved value  
				//���򣬰�iter_��user key��value����saved key��saved value  
				value_type = ikey.type;
				if (value_type == kTypeDeletion) {
					saved_key_.clear();
					ClearSavedValue();
				}
				else {
					Slice raw_value = iter_->value();
					if (saved_value_.capacity() > raw_value.size() + 1048576) {
						std::string empty;
						swap(empty, saved_value_);
					}//saveһֱ�ڱ䣬ֱ������iter��save��userkey��һ����˵��save�����µ�
					SaveKey(ExtractUserKey(iter_->key()), &saved_key_);
					saved_value_.assign(raw_value.data(), raw_value.size());
				}
			}
			iter_->Prev();
		} while (iter_->Valid());
	}

	if (value_type == kTypeDeletion) {
		// End
		valid_ = false;
		saved_key_.clear();
		ClearSavedValue();
		direction_ = kForward;
	}
	else {
		valid_ = true;
	}
}

void DBIter::Seek(const Slice& target) {
	direction_ = kForward; // ��ǰseek  
						   // ���saved value��saved key��������target����saved key  
	ClearSavedValue();
	saved_key_.clear();
	AppendInternalKey( // kValueTypeForSeek(1) > kDeleteType(0)  
		&saved_key_, ParsedInternalKey(target, sequence_, kValueTypeForSeek));
	iter_->Seek(saved_key_); // iter seek��saved key  
							 //���Զ�λ���Ϸ���iter������Ҫ����Delete��entry  
	if (iter_->Valid()) FindNextUserEntry(false, &saved_key_); //��false 
	else valid_ = false;
}

void DBIter::SeekToFirst() {
	direction_ = kForward; // ��ǰseek  
						   // ���saved value������iter_->SeekToFirst��Ȼ������Delete��entry  
	ClearSavedValue();
	iter_->SeekToFirst();
	if (iter_->Valid()) FindNextUserEntry(false, &saved_key_ /*��ʱ�洢*/);
	else valid_ = false;
}

void DBIter::SeekToLast() { // ����  
	direction_ = kReverse;
	ClearSavedValue();
	iter_->SeekToLast();
	FindPrevUserEntry();
}

}  // anonymous namespace

Iterator* NewDBIterator(
    DBImpl* db,
    const Comparator* user_key_comparator,
    Iterator* internal_iter,
    SequenceNumber sequence,
    uint32_t seed) {
  return new DBIter(db, user_key_comparator, internal_iter, sequence, seed);
}

}  // namespace leveldb
