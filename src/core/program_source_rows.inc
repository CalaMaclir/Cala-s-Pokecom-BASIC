// Included in program_store.cpp: row edits use the same storage transaction.
bool ProgramStore::new_program(ProgramSourceMode mode) {
    const auto prior=source_mode_;
    source_mode_=mode;
    if(!clear()){source_mode_=prior;return false;}
    return true;
}
bool ProgramStore::replace_source_rows(std::size_t first,std::size_t remove,
    const char* const* rows,std::size_t insert) {
    if(source_mode_!=ProgramSourceMode::Structured)return fail("STRUCTURED SOURCE REQUIRED");
    if(!ready())return false;
    if(first>count_ || remove>count_-first || insert>2 || remove>2 ||
       (insert && !rows))return fail("BAD SOURCE ROW EDIT");
    const std::size_t next_count=count_-remove+insert;
    if(next_count>line_capacity())return fail("PROGRAM FULL");
    for(std::size_t i=0;i<insert;++i)
        if(!rows[i] || std::strlen(rows[i])>line_length_capacity())
            return fail("LINE TOO LONG");
    RowEdit edit{first,remove,insert,rows};
    if(backend_==ProgramBackend::Sd) {
        Lease lease;if(!lease.locked)return fail(storage::last_error());
        BackendPtr<SdProgramStore> next(allocate_backend<SdProgramStore>());
        char work[80];std::size_t count=0,bytes=0;
        if(!next)return fail("OUT OF MEMORY");
        if(!new_work_name(work) || !snapshot(work,*this,*next,count,bytes,nullptr,false,nullptr,&edit))
            return false;
        if(!publish_sd(next.get(),count,bytes,filename_,true))return false;
        next.release();return true;
    }
    if(!ram_psram_) {
        // SRAM writes cannot fail after prevalidation; move only the suffix.
        std::memmove(ram_->lines+first+insert,ram_->lines+first+remove,
                     (count_-first-remove)*sizeof(ProgramLine));
        for(std::size_t j=0;j<insert;++j)
            std::strcpy(ram_->lines[first+j].text,rows[j]);
        bytes_=0;
        for(std::size_t j=0;j<next_count;++j) {
            ram_->lines[j].number=static_cast<std::int32_t>(j+1);
            bytes_+=std::strlen(ram_->lines[j].text)+1;
        }
        count_=next_count;dirty_=true;bump_revision();return true;
    }
    using Entries=std::unique_ptr<PsramProgramIndexEntry,decltype(&std::free)>;
    using Buffer=std::unique_ptr<char,decltype(&std::free)>;
    Entries old(static_cast<PsramProgramIndexEntry*>(std::malloc((count_+1)*sizeof(PsramProgramIndexEntry))),&std::free);
    Entries next(static_cast<PsramProgramIndexEntry*>(std::malloc((next_count+1)*sizeof(PsramProgramIndexEntry))),&std::free);
    Buffer backup(static_cast<char*>(std::malloc(2*kMaxPsramProgramLineLength)),&std::free);
    if(!old||!next||!backup)return fail("OUT OF MEMORY");
    if(count_&&!psram::read(ram_psram_base_,old.get(),count_*sizeof(PsramProgramIndexEntry)))
        return fail("PSRAM READ ERROR");
    bool used[kMaxPsramProgramLines]={};
    for(std::size_t i=0;i<count_;++i)
        if(i<first||i>=first+remove)used[old.get()[i].slot]=true;
    std::uint16_t slots[2]={};bool overwritten[2]={};
    for(std::size_t j=0;j<insert;++j) {
        std::size_t slot=0;while(slot<kMaxPsramProgramLines&&used[slot])++slot;
        if(slot==kMaxPsramProgramLines)return fail("PROGRAM FULL");
        used[slot]=true;slots[j]=static_cast<std::uint16_t>(slot);
        for(std::size_t i=first;i<first+remove;++i)
            if(old.get()[i].slot==slot)overwritten[j]=true;
        if(overwritten[j]&&!psram::read(psram_text_base()+slot*kMaxPsramProgramLineLength,
              backup.get()+j*kMaxPsramProgramLineLength,kMaxPsramProgramLineLength))
            return fail("PSRAM READ ERROR");
    }
    std::size_t at=0,bytes=0;
    for(std::size_t i=0;i<=count_;++i) {
        if(i==first)for(std::size_t j=0;j<insert;++j) {
            const auto length=std::strlen(rows[j]);
            next.get()[at]={static_cast<std::int32_t>(at+1),
                hash_line(static_cast<std::int32_t>(at+1),rows[j],length),
                slots[j],static_cast<std::uint16_t>(length)};
            ++at;bytes+=length+1;
        }
        if(i==count_)break;
        if(i>=first&&i<first+remove)continue;
        const char* body=nullptr;
        if(!psram_read_text(old.get()[i],body))return fail("PSRAM READ ERROR");
        auto entry=old.get()[i];entry.number=static_cast<std::int32_t>(at+1);
        entry.hash=hash_line(entry.number,body,entry.length);
        next.get()[at++]=entry;bytes+=entry.length+1;
    }
    bool ok=true;std::size_t written=0;
    for(;written<insert;++written)
        if(!psram_write_text(slots[written],rows[written],std::strlen(rows[written]))) {
            ++written;ok=false;break;
        }
    if(ok)ok=psram::write(ram_psram_base_,next.get(),next_count*sizeof(PsramProgramIndexEntry));
    if(!ok) {
        bool restored=true;
        for(std::size_t j=0;j<written;++j)if(overwritten[j])
            restored=psram::write(psram_text_base()+slots[j]*kMaxPsramProgramLineLength,
                backup.get()+j*kMaxPsramProgramLineLength,kMaxPsramProgramLineLength)&&restored;
        restored=psram::write(ram_psram_base_,old.get(),count_*sizeof(PsramProgramIndexEntry))&&restored;
        return fail(restored?"PSRAM WRITE ERROR":"PSRAM RECOVERY REQUIRED",!restored);
    }
    count_=next_count;bytes_=bytes;dirty_=true;bump_revision();error_="OK";return true;
}
