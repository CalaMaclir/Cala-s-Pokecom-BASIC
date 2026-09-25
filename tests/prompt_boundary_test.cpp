#include "line_editor.hpp"
#include "platform.hpp"
#include <cassert>
#include <string>
namespace {std::string output;int key=0;int column=7;}
namespace rmb::platform {
ConsoleMode get_console_mode(){return ConsoleMode::Both;}
bool terminal_console_enabled(){return true;}
void begin_command_input(){}
void end_command_input(){}
int get_char(){return key;}
int cursor_column(){return column;} int cursor_row(){return 38;}
int text_columns(){return 53;} int text_rows(){return 39;}
void set_cursor_position(int,int){} void scroll_text_rows(int){}
void screen_put_char(char){} void serial_put_char_raw(char){}
void serial_put_string_raw(const char*){}
void put_string(const char* s){output+=s;}
}
int main(){
 char input[224];
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
}
