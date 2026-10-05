#include "line_editor.hpp"
#include "platform.hpp"
#include "input_hotkeys.hpp"
#include "command_history.hpp"
#include <cassert>
#include <string>
#include <vector>
namespace {std::string output;std::string screen;int key=0;int column=7;std::vector<int> keys;std::size_t position=0;}
namespace rmb::platform {
ConsoleMode get_console_mode(){return ConsoleMode::Both;}
bool terminal_console_enabled(){return true;}
void begin_command_input(){}
void end_command_input(){}
int get_char(){return position<keys.size()?keys[position++]:key;}
int cursor_column(){return column;} int cursor_row(){return 38;}
int text_columns(){return 53;} int text_rows(){return 39;}
void set_cursor_position(int,int){} void scroll_text_rows(int){}
void screen_put_char(char c){screen+=c;} void serial_put_char_raw(char){}
void serial_put_string_raw(const char*){}
void put_string(const char* s){output+=s;}
}
int main(){
 char input[224];
 using namespace rmb::input_hotkeys;
 for (int shortcut : {Editor,Run,ControlCenter}) {
  keys={shortcut};position=0;output="BASIC> ";key='\n';
  assert(rmb::LineEditor::read(input,sizeof(input),nullptr,nullptr,true)==0);
  assert(rmb::LineEditor::last_special_key()==shortcut&&output=="BASIC> \r\n");
  keys={'P','R','I','N','T',' ','1',shortcut,'2','\n'};position=0;
  assert(rmb::LineEditor::read(input,sizeof(input),nullptr,nullptr,true)==8);
  assert(std::string(input)=="PRINT 12"&&rmb::LineEditor::last_special_key()==0);
  // Empty modal prompts explicitly do not opt in to BASIC workflow actions.
  keys={shortcut,'X','\n'};position=0;
  assert(rmb::LineEditor::read(input,sizeof(input))==1);
  assert(std::string(input)=="X"&&rmb::LineEditor::last_special_key()==0);
 }
 keys={'A',Outdent,Match,ShiftTab,'B','\n'};position=0;
 assert(rmb::LineEditor::read(input,sizeof(input),nullptr,nullptr,true)==2);
 assert(std::string(input)=="AB");
 rmb::CommandHistory history;history.append("PRINT 9");
 keys={0xb5,'\n'};position=0;
 assert(rmb::LineEditor::read(input,sizeof(input),&history,nullptr,true)==7);
 assert(std::string(input)=="PRINT 9");
 keys={'X',0xb1};position=0;
 assert(rmb::LineEditor::read(input,sizeof(input),nullptr,nullptr,true)==0);
 assert(rmb::LineEditor::last_special_key()==0xb1);
 // Wi-Fi passwords use the ordinary visible LineEditor path: cursor edits,
 // backspace, cancellation, Enter, and the 63-character field limit.
 screen.clear();keys={'a','b','c',0xb4,0x08,'Z','\n'};position=0;
 assert(rmb::LineEditor::read(input,64)==3 && std::string(input)=="aZc");
 assert(screen.find("abc")!=std::string::npos && screen.find("aZc")!=std::string::npos);
 keys.assign(70,'x');keys.push_back('\n');position=0;
 assert(rmb::LineEditor::read(input,64)==63 && std::strlen(input)==63);
 keys.clear();position=0;
 for(int i=0;i<10;++i){
  output="BASIC> ";key=0xd2;
  assert(rmb::LineEditor::read(input,sizeof(input))==0);
  assert(rmb::LineEditor::last_special_key()==key);
  output+="BASIC> ";assert(output=="BASIC> \r\nBASIC> ");
 }
 // F1-F10 already terminate the prompt in Repl::handle_quick_key. The
 // editor must not add an extra blank line (especially at the bottom row).
 for(int f:{0x81,0x82,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x90}){
  output="BASIC> ";key=f;rmb::LineEditor::read(input,sizeof(input));
  assert(rmb::LineEditor::last_special_key()==f);assert(output=="BASIC> ");
 }

 // Rename-style text entry starts with editable text already in the buffer.
 output="New: ";key=0x0a;
 assert(rmb::LineEditor::read(input,sizeof(input),nullptr,"OLD.BAS")==7);
 assert(std::string(input)=="OLD.BAS");
 assert(output=="New: \r\n");
}
