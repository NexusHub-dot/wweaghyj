#include "library.hpp"
#include <iostream>
using namespace pulse;
void check(bool ok,char const* why){if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F fn){bool threw=false;try{fn();}catch(std::exception const&){threw=true;}check(threw,"Expected rejection");}
int main(int argc,char** argv){
    if(argc!=2)return 2;
    auto directory=std::filesystem::absolute(argv[1])/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    try{
        std::filesystem::create_directories(directory);
        auto path=directory/"slot1.pulse",other=directory/"slot2.pulse";
        Tape first;first.levelName="First take";first.steps={{1.f/240,0,0,0,0}};
        Tape second=first;second.levelName="Replacement take";second.fixedUpdates=true;
        second.steps.push_back({1.f/240,1,0,1,0});Event press;press.down=true;second.events.push_back(press);
        saveRecordingFile(path,first);saveRecordingFile(other,first);saveRecordingFile(path,second);
        auto loaded=load(path);check(loaded.steps.size()==2 && loaded.events.size()==1 && loaded.levelName=="Replacement take","Re-recording did not replace selected saved slot");
        check(load(other).steps.size()==1,"Saving changed another slot");
        auto backup=path;backup+=".bak";check(load(backup).steps.size()==1,"Replacement did not preserve previous take");
        auto invalid=second;invalid.steps.clear();rejects([&]{saveRecordingFile(path,invalid);});
        check(load(path).steps.size()==2,"Failed save destroyed valid recording");
        archiveRecording(path);check(!std::filesystem::exists(path)&&std::filesystem::exists(deletedPath(path)),"Delete did not remove the playable slot");
        check(std::filesystem::exists(other)&&std::filesystem::exists(backup),"Delete touched other saved files");
        restoreRecording(path);check(load(path).events.size()==1,"Undo did not restore inputs");
        archiveRecording(path);saveRecordingFile(path,first);rejects([&]{restoreRecording(path);});
        check(load(path).steps.size()==1,"Undo overwrote a new recording");
        archiveRecording(path);restoreRecording(path);check(load(path).steps.size()==1,"Repeated delete/undo restored wrong take");
        bool preservedOlder=false;for(auto const& entry:std::filesystem::directory_iterator(directory))if(entry.path().filename().string().find(".deleted-")!=std::string::npos)preservedOlder=true;
        check(preservedOlder,"Second deletion lost the previously deleted take");
        check(duration(second)>duration(first),"Duration does not reflect replacement take");
        // Remove only the test files we just created, without recursive deletion.
        for(auto const& entry:std::filesystem::directory_iterator(directory))std::filesystem::remove(entry.path());
        std::filesystem::remove(directory);
        std::cout<<"PASS: disk replacement, selected-slot isolation, backup, failed-save preservation, delete/undo, repeated deletion\n";
        return 0;
    }catch(std::exception const& e){std::cerr<<"FAIL: "<<e.what()<<" (test files: "<<directory<<")\n";return 1;}
}
