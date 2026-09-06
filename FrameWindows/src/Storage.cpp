#include "Storage.hpp"
#include <Geode/Geode.hpp>
#include <fstream>
#include <sstream>
using namespace geode::prelude;
namespace fwl {
std::filesystem::path dataPath(std::string const& key) {
    return Mod::get()->getSaveDir() / "frame-windows" / (key + ".fwl.json");
}
bool save(Run const& r, std::string& error) {
    try {
        auto j = matjson::Value::object();
        j["schema"] = 2; j["tickRate"] = 240; j["fingerprint"] = r.fingerprint;
        j["measurementMode"]=r.nextClick?"next-control-survival":"reference-end-survival";j["clock"]="physics-steps";
        j["analysisVersion"]=4;
        j["modVersion"]=Mod::get()->getVersion().toVString();j["gdVersion"]="2.2081";
        j["levelName"] = r.levelName; j["endTick"] = r.endTick; j["completed"] = r.completed;
        j["pulseInputs"] = r.pulseInputs;
        j["macroRevision"] = r.macroRevision;
        j["seed"] = std::to_string(r.seed); j["environment"] = r.environment;
        j["inputs"] = matjson::Value::array();
        for (size_t k=0;k<r.inputs.size();++k) {
            auto const& in=r.inputs[k]; auto v=matjson::Value::object();
            v["tick"]=in.tick; v["player"]=in.player; v["down"]=in.down; v["x"]=in.x; v["y"]=in.y;
            v["wave"]=in.wave;
            if(k<r.windows.size()) {
                auto const& w=r.windows[k]; v["early"]=w.early; v["late"]=w.late;
                v["measured"]=w.measured; v["overflow"]=w.overflow;
                v["earlyFailureTick"]=w.earlyFailureTick;v["lateFailureTick"]=w.lateFailureTick;
                v["unstable"]=w.unstable;v["checkpointFallback"]=w.checkpointFallback;
                if(w.measured){v["testedEarliest"]=in.tick-w.early;v["testedLatest"]=in.tick+w.late;v["nominalMs"]=w.nominalMilliseconds();}
            }
            j["inputs"].push(v);
        }
        j["poses"]=matjson::Value::array();
        for(auto const& p:r.poses) {
            auto v=matjson::Value::object(); v["tick"]=p.tick; v["values"]=matjson::Value::array();
            for(double x:p.values) v["values"].push(x);
            if(p.dual)v["dual"]=*p.dual;
            j["poses"].push(v);
        }
        auto path=dataPath(r.fingerprint), temp=path; temp += ".tmp";
        std::filesystem::create_directories(path.parent_path());
        { std::ofstream out(temp,std::ios::binary); out << j.dump(2); if(!out) throw std::runtime_error("Unable to write run file"); }
        // Windows replace without deleting the existing reference first.
        if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Unable to replace run file");
        return true;
    } catch(std::exception const& e) { error=e.what(); return false; }
}
std::optional<Run> load(std::string const& fingerprint,std::string& error) {
    try {
        auto path=dataPath(fingerprint); if(!std::filesystem::exists(path)) return {};
        if(std::filesystem::file_size(path)>32*1024*1024) throw std::runtime_error("Run file exceeds 32 MB");
        std::ifstream in(path); std::string text((std::istreambuf_iterator<char>(in)),{});
        auto parsed=matjson::parse(text); if(!parsed) throw std::runtime_error("Invalid run JSON");
        auto& j=parsed.unwrap();
        auto integer=[&](matjson::Value const& v,char const* key) { auto n=v[key].asInt(); if(!n) throw std::runtime_error("Invalid integer in run"); return n.unwrap(); };
        if(integer(j,"schema")!=2 || integer(j,"tickRate")!=240) throw std::runtime_error("This reference uses the old progress-counter clock. Record a new reference with F6.");
        Run r; r.fingerprint=j["fingerprint"].asString().unwrapOr("");
        if(r.fingerprint!=fingerprint) throw std::runtime_error("Wrong level fingerprint");
        r.levelName=j["levelName"].asString().unwrapOr(""); r.environment=j["environment"].asString().unwrapOr("");
        r.endTick=static_cast<int>(integer(j,"endTick")); r.completed=j["completed"].asBool().unwrapOr(false);
        r.pulseInputs=j["pulseInputs"].asBool().unwrapOr(false);
        r.macroRevision=j["macroRevision"].asString().unwrapOr("");
        auto scope=j["measurementMode"].asString().unwrapOr("");
        r.nextClick=scope=="next-click-survival" || scope=="next-control-survival";
        bool currentAnalysis=j["analysisVersion"].asInt().unwrapOr(0)==4;
        if(r.endTick<=0 || r.endTick>240*60*60*4) throw std::runtime_error("Invalid reference duration");
        r.seed=std::stoull(j["seed"].asString().unwrapOr("0"));
        auto inputs=j["inputs"].asArray(); if(!inputs || inputs.unwrap().size()>100000) throw std::runtime_error("Invalid inputs");
        int previous=-1;
        for(auto const& v:inputs.unwrap()) {
            Input a; a.tick=static_cast<int>(integer(v,"tick")); a.player=static_cast<int>(integer(v,"player"));
            a.down=v["down"].asBool().unwrapOr(false); a.x=static_cast<float>(v["x"].asDouble().unwrapOr(0)); a.y=static_cast<float>(v["y"].asDouble().unwrapOr(0));
            a.wave=v["wave"].asBool().unwrapOr(false);
            if(a.tick<previous || a.tick<0 || a.tick>=r.endTick || a.player<0 || a.player>1 || !std::isfinite(a.x) || !std::isfinite(a.y)) throw std::runtime_error("Invalid input timeline");
            previous=a.tick; r.inputs.push_back(a);
            Window w; w.early=static_cast<int>(v["early"].asInt().unwrapOr(0)); w.late=static_cast<int>(v["late"].asInt().unwrapOr(0));
            w.measured=v["measured"].asBool().unwrapOr(false); w.overflow=v["overflow"].asBool().unwrapOr(false);
            w.earlyFailureTick=static_cast<int>(v["earlyFailureTick"].asInt().unwrapOr(-1));
            w.lateFailureTick=static_cast<int>(v["lateFailureTick"].asInt().unwrapOr(-1));
            w.unstable=v["unstable"].asBool().unwrapOr(false);
            w.checkpointFallback=v["checkpointFallback"].asBool().unwrapOr(false);
            if(w.early<0 || w.late<0 || w.early+w.late>20 || (w.measured && w.overflow!=(w.width()>20))) throw std::runtime_error("Invalid measured window");
            r.windows.push_back(currentAnalysis?w:Window{});
        }
        auto poses=j["poses"].asArray(); if(!poses || poses.unwrap().size()>100000) throw std::runtime_error("Missing validation poses");
        for(auto const& v:poses.unwrap()) {
            Pose p; p.tick=static_cast<int>(integer(v,"tick"));
            auto values=v["values"].asArray(); if(!values || values.unwrap().size()!=6) throw std::runtime_error("Invalid validation pose");
            for(int k=0;k<6;++k) { p.values[k]=values.unwrap()[k].asDouble().unwrapOr(NAN); if(!std::isfinite(p.values[k])) throw std::runtime_error("Invalid pose value"); }
            if(auto dual=v["dual"].asBool())p.dual=dual.unwrap();
            if(p.tick<0 || p.tick>r.endTick || (!r.poses.empty() && p.tick<=r.poses.back().tick)) throw std::runtime_error("Invalid pose order");
            r.poses.push_back(p);
        }
        if(!currentAnalysis)error="Reference loaded. Re-analyze to replace the old widths with the new timing checks.";
        return r;
    }catch(std::exception const& e) { error=e.what(); return {}; }
}
bool exportCSV(Run const& r,std::string& error) {
    try {
        auto path=Mod::get()->getSaveDir()/"frame-windows"/(r.fingerprint+".csv");
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path);
        out << "input,tick,seconds,player,action,early_ticks,late_ticks,window_ticks,censored_above_20,measured,tested_earliest,tested_latest,nominal_ms,width_is_lower_bound,scope,endpoint_tick,early_failure_tick,late_failure_tick,player_mode,unstable,checkpoint_fallback\n";
        for(size_t i=0;i<r.inputs.size();++i) {
            auto const& a=r.inputs[i]; Window w=i<r.windows.size()?r.windows[i]:Window{};
            out << i+1 << ',' << a.tick << ',' << a.tick/240.0 << ',' << a.player+1 << ',' << (a.down?"press":"release") << ',';
            if(w.measured) out << w.early << ',' << w.late << ',' << w.width(); else out << ",,";
            out << ',' << w.overflow << ',' << w.measured;
            if(w.measured)out<<','<<a.tick-w.early<<','<<a.tick+w.late<<','<<w.nominalMilliseconds()<<','<<w.overflow;
            else out<<",,,,";
            out<<','<<(r.nextClick?(a.wave?"next-direction-change":"next-click"):"full-macro")<<','<<trialEndpoint(r,i,r.nextClick)<<',';
            if(w.measured && w.earlyFailureTick>=0)out<<w.earlyFailureTick;
            out<<',';if(w.measured && w.lateFailureTick>=0)out<<w.lateFailureTick;
            out<<','<<(a.wave?"wave":"other")<<','<<w.unstable<<','<<w.checkpointFallback<<'\n';
        }
        if(!out) throw std::runtime_error("Unable to export CSV"); return true;
    }catch(std::exception const& e) { error=e.what(); return false; }
}
}
