// Copyright (c) 2015-present, Qihoo, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.

#include "include/pika_geo.h"

#include <algorithm>

#include "pstd/include/pstd_string.h"

#include "include/pika_geohash_helper.h"

void GeoAddCmd::DoInitial() {
  if (!CheckArg(argv_.size())) {
    res_.SetRes(CmdRes::kWrongNum, kCmdNameGeoAdd);
    return;
  }
  size_t argc = argv_.size();
  if ((argc - 2) % 3 != 0) {
    res_.SetRes(CmdRes::kWrongNum, kCmdNameGeoAdd);
    return;
  }
  key_ = argv_[1];
  pos_.clear();
  struct GeoPoint point;
  double longitude;
  double latitude;
  for (size_t index = 2; index < argc; index += 3) {
    if (pstd::string2d(argv_[index].data(), argv_[index].size(), &longitude) == 0) {
      res_.SetRes(CmdRes::kInvalidFloat);
      return;
    }
    if (pstd::string2d(argv_[index + 1].data(), argv_[index + 1].size(), &latitude) == 0) {
      res_.SetRes(CmdRes::kInvalidFloat);
      return;
    }
    point.member = argv_[index + 2];
    point.longitude = longitude;
    point.latitude = latitude;
    pos_.push_back(point);
  }
}

void GeoAddCmd::Do() {
  std::vector<storage::ScoreMember> score_members;
  for (const auto& geo_point : pos_) {
    // Convert coordinates to geohash
    GeoHashBits hash;
    geohashEncodeWGS84(geo_point.longitude, geo_point.latitude, GEO_STEP_MAX, &hash);
    GeoHashFix52Bits bits = geohashAlign52Bits(hash);
    // Convert uint64 to double
    double score;
    std::string str_bits = std::to_string(bits);
    pstd::string2d(str_bits.data(), str_bits.size(), &score);
    score_members.push_back({score, geo_point.member});
  }
  int32_t count = 0;
  rocksdb::Status s = db_->storage()->ZAdd(key_, score_members, &count);
  if (s.ok()) {
    res_.AppendInteger(count);
  } else if (s_.IsInvalidArgument()) {
    res_.SetRes(CmdRes::kMultiKey);
  } else {
    res_.SetRes(CmdRes::kErrOther, s.ToString());
  }
}

void GeoPosCmd::DoInitial() {
  if (!CheckArg(argv_.size())) {
    res_.SetRes(CmdRes::kWrongNum, kCmdNameGeoPos);
    return;
  }
  key_ = argv_[1];
  members_.clear();
  size_t pos = 2;
  while (pos < argv_.size()) {
    members_.push_back(argv_[pos++]);
  }
}

void GeoPosCmd::Do() {
  double score = 0.0;
  res_.AppendArrayLenUint64(members_.size());
  for (const auto& member : members_) {
    rocksdb::Status s = db_->storage()->ZScore(key_, member, &score);
    if (s.ok()) {
      double xy[2];
      GeoHashBits hash = {.bits = static_cast<uint64_t>(score), .step = GEO_STEP_MAX};
      geohashDecodeToLongLatWGS84(hash, xy);

      res_.AppendArrayLen(2);
      char longitude[32];
      int64_t len = pstd::d2string(longitude, sizeof(longitude), xy[0]);
      res_.AppendStringLen(len);
      res_.AppendContent(longitude);

      char latitude[32];
      len = pstd::d2string(latitude, sizeof(latitude), xy[1]);
      res_.AppendStringLen(len);
      res_.AppendContent(latitude);

    } else if (s.IsNotFound()) {
      res_.AppendStringLen(-1);
      continue;
    } else if (s_.IsInvalidArgument()) {
      res_.SetRes(CmdRes::kMultiKey);
      continue;
    } else {
      res_.SetRes(CmdRes::kErrOther, s.ToString());
      continue;
    }
  }
}

static double length_converter(double meters, const std::string& unit) {
  if (unit == "m") {
    return meters;
  } else if (unit == "km") {
    return meters / 1000;
  } else if (unit == "ft") {
    return meters / 0.3048;
  } else if (unit == "mi") {
    return meters / 1609.34;
  } else {
    return -1;
  }
}

static bool check_unit(const std::string& unit) {
  return unit == "m" || unit == "km" || unit == "ft" || unit == "mi";
}

void GeoDistCmd::DoInitial() {
  if (!CheckArg(argv_.size())) {
    res_.SetRes(CmdRes::kWrongNum, kCmdNameGeoDist);
    return;
  }
  if (argv_.size() < 4) {
    res_.SetRes(CmdRes::kWrongNum, kCmdNameGeoDist);
    return;
  } else if (argv_.size() > 5) {
    res_.SetRes(CmdRes::kSyntaxErr);
    return;
  }
  key_ = argv_[1];
  first_pos_ = argv_[2];
  second_pos_ = argv_[3];
  if (argv_.size() == 5) {
    unit_ = argv_[4];
  } else {
    unit_ = "m";
  }
  if (!check_unit(unit_)) {
    res_.SetRes(CmdRes::kErrOther, "unsupported unit provided. please use m, km, ft, mi");
    return;
  }
}

void GeoDistCmd::Do() {
  double first_score = 0.0;
  double second_score = 0.0;
  double first_xy[2];
  double second_xy[2];
  rocksdb::Status s = db_->storage()->ZScore(key_, first_pos_, &first_score);
  if (s.ok()) {
    GeoHashBits hash = {.bits = static_cast<uint64_t>(first_score), .step = GEO_STEP_MAX};
    geohashDecodeToLongLatWGS84(hash, first_xy);
  } else if (s.IsNotFound()) {
    res_.AppendStringLen(-1);
    return;
  } else if (s_.IsInvalidArgument()) {
    res_.SetRes(CmdRes::kMultiKey);
    return;
  } else {
    res_.SetRes(CmdRes::kErrOther, s.ToString());
    return;
  }

  s = db_->storage()->ZScore(key_, second_pos_, &second_score);
  if (s.ok()) {
    GeoHashBits hash = {.bits = static_cast<uint64_t>(second_score), .step = GEO_STEP_MAX};
    geohashDecodeToLongLatWGS84(hash, second_xy);
  } else if (s.IsNotFound()) {
    res_.AppendStringLen(-1);
    return;
  } else {
    res_.SetRes(CmdRes::kErrOther, s.ToString());
    return;
  }

  double distance = geohashGetDistance(first_xy[0], first_xy[1], second_xy[0], second_xy[1]);
  distance = length_converter(distance, unit_);
  char buf[32];
  snprintf(buf, sizeof(buf), "%.4f", distance);
  res_.AppendStringLenUint64(strlen(buf));
  res_.AppendContent(buf);
}

void GeoHashCmd::DoInitial() {
  if (!CheckArg(argv_.size())) {
    res_.SetRes(CmdRes::kWrongNum, kCmdNameGeoHash);
    return;
  }
  key_ = argv_[1];
  members_.clear();
  size_t pos = 2;
  while (pos < argv_.size()) {
    members_.push_back(argv_[pos++]);
  }
}

void GeoHashCmd::Do() {
  const char* geoalphabet = "0123456789bcdefghjkmnpqrstuvwxyz";
  res_.AppendArrayLenUint64(members_.size());
  for (const auto& member : members_) {
    double score = 0.0;
    rocksdb::Status s = db_->storage()->ZScore(key_, member, &score);
    if (s.ok()) {
      double xy[2];
      GeoHashBits hash = {.bits = static_cast<uint64_t>(score), .step = GEO_STEP_MAX};
      geohashDecodeToLongLatWGS84(hash, xy);
      GeoHashRange r[2];
      GeoHashBits encode_hash;
      r[0].min = -180;
      r[0].max = 180;
      r[1].min = -90;
      r[1].max = 90;
      geohashEncode(&r[0], &r[1], xy[0], xy[1], 26, &encode_hash);

      char buf[12];
      int i;
      for (i = 0; i < 11; i++) {
        uint64_t idx = (encode_hash.bits >> (52 - ((i + 1) * 5))) & 0x1f;
        buf[i] = geoalphabet[idx];
      }
      buf[11] = '\0';
      res_.AppendStringLen(11);
      res_.AppendContent(buf);
      continue;
    } else if (s.IsNotFound()) {
      res_.AppendStringLen(-1);
      continue;
    } else if (s_.IsInvalidArgument()) {
      res_.SetRes(CmdRes::kMultiKey);
      continue;
    } else {
      res_.SetRes(CmdRes::kErrOther, s.ToString());
      continue;
    }
  }
}

static bool sort_distance_asc(const NeighborPoint& pos1, const NeighborPoint& pos2) {
  return pos1.distance < pos2.distance;
}

static bool sort_distance_desc(const NeighborPoint& pos1, const NeighborPoint& pos2) {
  return pos1.distance > pos2.distance;
}

static void GetAllNeighbors(const std::shared_ptr<DB>& db, std::string& key, GeoRange& range, CmdRes& res) {
  rocksdb::Status s;
  double longitude = range.longitude;
  double latitude = range.latitude;
  double distance = range.distance;
  int count_limit = 0;
  // Convert other units to meters
  if (range.unit == "m") {
    distance = distance;
  } else if (range.unit == "km") {
    distance = distance * 1000;
  } else if (range.unit == "ft") {
    distance = distance * 0.3048;
  } else if (range.unit == "mi") {
    distance = distance * 1609.34;
  } else {
    distance = -1;
  }
  // Search the zset for all matching points
  GeoHashRadius georadius = geohashGetAreasByRadiusWGS84(longitude, latitude, distance);
  GeoHashBits neighbors[9];
  neighbors[0] = georadius.hash;
  neighbors[1] = georadius.neighbors.north;
  neighbors[2] = georadius.neighbors.south;
  neighbors[3] = georadius.neighbors.east;
  neighbors[4] = georadius.neighbors.west;
  neighbors[5] = georadius.neighbors.north_east;
  neighbors[6] = georadius.neighbors.north_west;
  neighbors[7] = georadius.neighbors.south_east;
  neighbors[8] = georadius.neighbors.south_west;

  // For each neighbor, get all the matching
  // members and add them to the potential result list.
  std::vector<NeighborPoint> result;
  size_t last_processed = 0;
  for (size_t i = 0; i < sizeof(neighbors) / sizeof(*neighbors); i++) {
    GeoHashFix52Bits min;
    GeoHashFix52Bits max;
    if (HASHISZERO(neighbors[i])) {
      continue;
    }
    min = geohashAlign52Bits(neighbors[i]);
    neighbors[i].bits++;
    max = geohashAlign52Bits(neighbors[i]);
    // When a huge Radius (in the 5000 km range or more) is used,
    // adjacent neighbors can be the same, so need to remove duplicated elements
    if ((last_processed != 0) && neighbors[i].bits == neighbors[last_processed].bits &&
        neighbors[i].step == neighbors[last_processed].step) {
      continue;
    }
    std::vector<storage::ScoreMember> score_members;
    s = db->storage()->ZRangebyscore(key, static_cast<double>(min), static_cast<double>(max), true, true, &score_members);
    if (!s.ok() && !s.IsNotFound()) {
      res.SetRes(CmdRes::kErrOther, s.ToString());
      return;
    }
    // Insert into result only if the point is within the search area.
    for (auto & score_member : score_members) {
      double xy[2];
      double real_distance = 0.0;
      GeoHashBits hash = {.bits = static_cast<uint64_t>(score_member.score), .step = GEO_STEP_MAX};
      geohashDecodeToLongLatWGS84(hash, xy);
      if (geohashGetDistanceIfInRadiusWGS84(longitude, latitude, xy[0], xy[1], distance, &real_distance) != 0) {
        NeighborPoint item;
        item.member = score_member.member;
        item.score = score_member.score;
        item.distance = real_distance;
        result.push_back(item);
      }
    }
    last_processed = i;
  }

  // If using the count opiton
  if (range.count) {
    count_limit = static_cast<int32_t>(result.size() < range.count_limit ? result.size() : range.count_limit);
  } else {
    count_limit = static_cast<int32_t>(result.size());
  }
  // If using sort option
  if (range.sort == Asc) {
    std::sort(result.begin(), result.end(), sort_distance_asc);
  } else if (range.sort == Desc) {
    std::sort(result.begin(), result.end(), sort_distance_desc);
  }

  if (range.store || range.storedist) {
    // Target key, create a sorted set with the results.
    std::vector<storage::ScoreMember> score_members;
    for (int i = 0; i < count_limit; ++i) {
      double distance = length_converter(result[i].distance, range.unit);
      double score = range.store ? result[i].score : distance;
      score_members.push_back({score, result[i].member});
    }
    int32_t count = 0;
    s = db->storage()->ZAdd(range.storekey, score_members, &count);
    if (!s.ok()) {
      res.SetRes(CmdRes::kErrOther, s.ToString());
      return;
    }
    res.AppendInteger(count_limit);
    return;
  } else {
    // No target key, return results to user.

    // For each the result
    res.AppendArrayLen(count_limit);
    for (int i = 0; i < count_limit; ++i) {
      if (range.option_num != 0) {
        res.AppendArrayLen(range.option_num + 1);
      }
      // Member
      res.AppendStringLenUint64(result[i].member.size());
      res.AppendContent(result[i].member);

      // If using withdist option
      if (range.withdist) {
        double xy[2];
        GeoHashBits hash = {.bits = static_cast<uint64_t>(result[i].score), .step = GEO_STEP_MAX};
        geohashDecodeToLongLatWGS84(hash, xy);
        double distance = geohashGetDistance(longitude, latitude, xy[0], xy[1]);
        distance = length_converter(distance, range.unit);
        char buf[32];
        snprintf(buf, sizeof(buf), "%.4f", distance);
        res.AppendStringLenUint64(strlen(buf));
        res.AppendContent(buf);
      }
      // If using withhash option
      if (range.withhash) {
        res.AppendInteger(static_cast<int64_t>(result[i].score));
      }
      // If using withcoord option
      if (range.withcoord) {
        res.AppendArrayLen(2);
        double xy[2];
        GeoHashBits hash = {.bits = static_cast<uint64_t>(result[i].score), .step = GEO_STEP_MAX};
        geohashDecodeToLongLatWGS84(hash, xy);

        char longitude[32];
        int64_t len = pstd::d2string(longitude, sizeof(longitude), xy[0]);
        res.AppendStringLen(len);
        res.AppendContent(longitude);

        char latitude[32];
        len = pstd::d2string(latitude, sizeof(latitude), xy[1]);
        res.AppendStringLen(len);
        res.AppendContent(latitude);
      }
    }
  }
}

void GeoRadiusCmd::DoInitial() {
  if (!CheckArg(argv_.size())) {
    res_.SetRes(CmdRes::kWrongNum, kCmdNameGeoRadius);
    return;
  }
  key_ = argv_[1];
  pstd::string2d(argv_[2].data(), argv_[2].size(), &range_.longitude);
  pstd::string2d(argv_[3].data(), argv_[3].size(), &range_.latitude);
  pstd::string2d(argv_[4].data(), argv_[4].size(), &range_.distance);
  range_.unit = argv_[5];
  if (!check_unit(range_.unit)) {
    res_.SetRes(CmdRes::kErrOther, "unsupported unit provided. please use m, km, ft, mi");
    return;
  }
  size_t pos = 6;
  while (pos < argv_.size()) {
    if (strcasecmp(argv_[pos].c_str(), "withdist") == 0) {
      range_.withdist = true;
      range_.option_num++;
    } else if (strcasecmp(argv_[pos].c_str(), "withhash") == 0) {
      range_.withhash = true;
      range_.option_num++;
    } else if (strcasecmp(argv_[pos].c_str(), "withcoord") == 0) {
      range_.withcoord = true;
      range_.option_num++;
    } else if (strcasecmp(argv_[pos].c_str(), "count") == 0) {
      range_.count = true;
      if (argv_.size() < (pos + 2)) {
        res_.SetRes(CmdRes::kSyntaxErr);
        return;
      }
      std::string str_count = argv_[++pos];
      for (auto s : str_count) {
        if (isdigit(s) == 0) {
          res_.SetRes(CmdRes::kErrOther, "value is not an integer or out of range");
          return;
        }
      }
      range_.count_limit = std::stoi(str_count);
    } else if (strcasecmp(argv_[pos].c_str(), "store") == 0) {
      range_.store = true;
      if (argv_.size() < (pos + 2)) {
        res_.SetRes(CmdRes::kSyntaxErr);
        return;
      }
      range_.storekey = argv_[++pos];
    } else if (strcasecmp(argv_[pos].c_str(), "storedist") == 0) {
      range_.storedist = true;
      if (argv_.size() < (pos + 2)) {
        res_.SetRes(CmdRes::kSyntaxErr);
        return;
      }
      range_.storekey = argv_[++pos];
    } else if (strcasecmp(argv_[pos].c_str(), "asc") == 0) {
      range_.sort = Asc;
    } else if (strcasecmp(argv_[pos].c_str(), "desc") == 0) {
      range_.sort = Desc;
    } else {
      res_.SetRes(CmdRes::kSyntaxErr);
      return;
    }
    pos++;
  }
  if (range_.store && (range_.withdist || range_.withcoord || range_.withhash)) {
    res_.SetRes(CmdRes::kErrOther,
                "STORE option in GEORADIUS is not compatible with WITHDIST, WITHHASH and WITHCOORDS options");
    return;
  }
}

void GeoRadiusCmd::Do() { GetAllNeighbors(db_, key_, range_, this->res_); }

void GeoRadiusByMemberCmd::DoInitial() {
  if (!CheckArg(argv_.size())) {
    res_.SetRes(CmdRes::kWrongNum, kCmdNameGeoRadius);
    return;
  }
  key_ = argv_[1];
  range_.member = argv_[2];
  pstd::string2d(argv_[3].data(), argv_[3].size(), &range_.distance);
  range_.unit = argv_[4];
  if (!check_unit(range_.unit)) {
    res_.SetRes(CmdRes::kErrOther, "unsupported unit provided. please use m, km, ft, mi");
    return;
  }
  size_t pos = 5;
  while (pos < argv_.size()) {
    if (strcasecmp(argv_[pos].c_str(), "withdist") == 0) {
      range_.withdist = true;
      range_.option_num++;
    } else if (strcasecmp(argv_[pos].c_str(), "withhash") == 0) {
      range_.withhash = true;
      range_.option_num++;
    } else if (strcasecmp(argv_[pos].c_str(), "withcoord") == 0) {
      range_.withcoord = true;
      range_.option_num++;
    } else if (strcasecmp(argv_[pos].c_str(), "count") == 0) {
      range_.count = true;
      if (argv_.size() < (pos + 2)) {
        res_.SetRes(CmdRes::kSyntaxErr);
        return;
      }
      std::string str_count = argv_[++pos];
      for (auto s : str_count) {
        if (isdigit(s) == 0) {
          res_.SetRes(CmdRes::kErrOther, "value is not an integer or out of range");
          return;
        }
      }
      range_.count_limit = std::stoi(str_count);
    } else if (strcasecmp(argv_[pos].c_str(), "store") == 0) {
      range_.store = true;
      if (argv_.size() < (pos + 2)) {
        res_.SetRes(CmdRes::kSyntaxErr);
        return;
      }
      range_.storekey = argv_[++pos];
    } else if (strcasecmp(argv_[pos].c_str(), "storedist") == 0) {
      range_.storedist = true;
      if (argv_.size() < (pos + 2)) {
        res_.SetRes(CmdRes::kSyntaxErr);
        return;
      }
      range_.storekey = argv_[++pos];
    } else if (strcasecmp(argv_[pos].c_str(), "asc") == 0) {
      range_.sort = Asc;
    } else if (strcasecmp(argv_[pos].c_str(), "desc") == 0) {
      range_.sort = Desc;
    } else {
      res_.SetRes(CmdRes::kSyntaxErr);
      return;
    }
    pos++;
  }
  if (range_.store && (range_.withdist || range_.withcoord || range_.withhash)) {
    res_.SetRes(CmdRes::kErrOther,
                "STORE option in GEORADIUS is not compatible with WITHDIST, WITHHASH and WITHCOORDS options");
    return;
  }
}

void GeoRadiusByMemberCmd::Do() {
  double score = 0.0;
  rocksdb::Status s = db_->storage()->ZScore(key_, range_.member, &score);
  if (s.ok()) {
    double xy[2];
    GeoHashBits hash = {.bits = static_cast<uint64_t>(score), .step = GEO_STEP_MAX};
    geohashDecodeToLongLatWGS84(hash, xy);
    range_.longitude = xy[0];
    range_.latitude = xy[1];
  }
  GetAllNeighbors(db_, key_, range_, this->res_);
}
