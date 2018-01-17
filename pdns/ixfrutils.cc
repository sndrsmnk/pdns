/*
 * This file is part of PowerDNS or dnsdist.
 * Copyright -- PowerDNS.COM B.V. and its contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * In addition, for the avoidance of any doubt, permission is granted to
 * link this program with OpenSSL and to (re)distribute the binaries
 * produced as the result of such linking.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <dirent.h>
#include <errno.h>
#include "ixfrutils.hh"
#include "sstuff.hh"
#include "dnssecinfra.hh"
#include "zoneparser-tng.hh"
#include "dnsparser.hh"

uint32_t getSerialFromMaster(const ComboAddress& master, const DNSName& zone, shared_ptr<SOARecordContent>& sr, const TSIGTriplet& tt)
{
  vector<uint8_t> packet;
  DNSPacketWriter pw(packet, zone, QType::SOA);
  if(!tt.algo.empty()) {
    TSIGRecordContent trc;
    trc.d_algoName = tt.algo;
    trc.d_time = time(nullptr);
    trc.d_fudge = 300;
    trc.d_origID=ntohs(pw.getHeader()->id);
    trc.d_eRcode=0;
    addTSIG(pw, trc, tt.name, tt.secret, "", false);
  }

  Socket s(master.sin4.sin_family, SOCK_DGRAM);
  s.connect(master);
  string msg((const char*)&packet[0], packet.size());
  s.writen(msg);

  string reply;
  s.read(reply);
  MOADNSParser mdp(false, reply);
  if(mdp.d_header.rcode) {
    throw std::runtime_error("Unable to retrieve SOA serial from master '"+master.toStringWithPort()+"': "+RCode::to_s(mdp.d_header.rcode));
  }
  for(const auto& r: mdp.d_answers) {
    if(r.first.d_type == QType::SOA) {
      sr = getRR<SOARecordContent>(r.first);
      if(sr != nullptr) {
        return sr->d_st.serial;
      }
    }
  }
  return 0;
}

uint32_t getSerialsFromDir(const std::string& dir)
{
  uint32_t ret=0;
  DIR* dirhdl=opendir(dir.c_str());
  if(!dirhdl)
    throw runtime_error("Could not open IXFR directory '" + dir + "': " + strerror(errno));
  struct dirent *entry;

  while((entry = readdir(dirhdl))) {
    uint32_t num = atoi(entry->d_name);
    if(std::to_string(num) == entry->d_name)
      ret = max(num, ret);
  }
  closedir(dirhdl);
  return ret;
}

uint32_t getSerialFromRecords(const records_t& records, DNSRecord& soaret)
{
  DNSName root(".");
  uint16_t t=QType::SOA;

  auto found = records.equal_range(tie(root, t));

  for(auto iter = found.first; iter != found.second; ++iter) {
    auto soa = std::dynamic_pointer_cast<SOARecordContent>(iter->d_content);
    soaret = *iter;
    return soa->d_st.serial;
  }
  return 0;
}

void writeZoneToDisk(const records_t& records, const DNSName& zone, const std::string& directory)
{
  DNSRecord soa;
  int serial = getSerialFromRecords(records, soa);
  string fname=directory +"/"+std::to_string(serial);
  FILE* fp=fopen((fname+".partial").c_str(), "w");
  if(!fp)
    throw runtime_error("Unable to open file '"+fname+".partial' for writing: "+string(strerror(errno)));

  records_t soarecord;
  soarecord.insert(soa);
  fprintf(fp, "$ORIGIN %s\n", zone.toString().c_str());
  for(const auto& outer : {soarecord, records, soarecord} ) {
    for(const auto& r: outer) {
      fprintf(fp, "%s\tIN\t%s\t%s\n",
          r.d_name.isRoot() ? "@" :  r.d_name.toStringNoDot().c_str(),
          DNSRecordContent::NumberToType(r.d_type).c_str(),
          r.d_content->getZoneRepresentation().c_str());
    }
  }
  fclose(fp);
  rename( (fname+".partial").c_str(), fname.c_str());
}

void loadZoneFromDisk(records_t& records, const string& fname, const DNSName& zone)
{
  ZoneParserTNG zpt(fname, zone);

  DNSResourceRecord rr;
  bool seenSOA=false;
  unsigned int nrecords=0;
  while(zpt.get(rr)) {
    ++nrecords;
    if(rr.qtype.getCode() == QType::CNAME && rr.content.empty())
      rr.content=".";
    rr.qname = rr.qname.makeRelative(zone);

    if(rr.qtype.getCode() != QType::SOA || seenSOA==false)
      records.insert(DNSRecord(rr));
    if(rr.qtype.getCode() == QType::SOA) {
      seenSOA=true;
    }
  }
  cout<<"Parsed "<<nrecords<<" records"<<endl;
  if(rr.qtype.getCode() == QType::SOA && seenSOA) {
    cout<<"Zone was complete (SOA at end)"<<endl;
  }
  else  {
    records.clear();
    throw runtime_error("Zone not complete!");
  }
}

/*
 * Load the zone `zone` from `fname` and put the first found SOA into `soa`
 * Does NOT check for nullptr
 */
void loadSOAFromDisk(const DNSName& zone, const string& fname, shared_ptr<SOARecordContent>& soa)
{
  ZoneParserTNG zpt(fname, zone);
  DNSResourceRecord rr;

  while(zpt.get(rr)) {
    if (rr.qtype == QType::SOA) {
      soa = getRR<SOARecordContent>(DNSRecord(rr));
      return;
    }
  }
}
