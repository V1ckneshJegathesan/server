/*****************************************************************************
Copyright (c) 2019, MariaDB Corporation.
*****************************************************************************/

#ifndef _clustrix_connection_h
#define _clustrix_connection_h

#ifdef USE_PRAGMA_INTERFACE
#pragma interface     /* gcc class implementation */
#endif

#define MYSQL_SERVER 1
#include "my_global.h"
#include "m_string.h"
#include "mysql.h"
#include "sql_common.h"
#include "my_base.h"
#include "mysqld_error.h"
#include "my_bitmap.h"
#include "handler.h"

#define CLUSTRIX_SERVER_REQUEST 30

class clustrix_connection
{
private:
# define COMMAND_BUFFER_SIZE_INCREMENT 1024
# define COMMAND_BUFFER_SIZE_INCREMENT_BITS 10

  MYSQL clustrix_net;
  MYSQL_RES *results;
  uchar *command_buffer;
  size_t command_buffer_length;
  size_t command_length;

  uchar *reply_buffer;
  size_t reply_length;

  bool has_transaction;
  bool has_statement_trans;

public:
  ulonglong last_insert_id;
  clustrix_connection()
    : command_buffer(NULL), command_buffer_length(0), command_length(0)
  {
    DBUG_ENTER("clustrix_connection::clustrix_connection");
    memset(&clustrix_net, 0, sizeof(MYSQL));
    has_statement_trans = FALSE;
    has_transaction = FALSE;
    DBUG_VOID_RETURN;
  }

  ~clustrix_connection()
  {
    DBUG_ENTER("clustrix_connection::~clustrix_connection");
    if (is_connected())
      disconnect(TRUE);

    if (command_buffer)
      my_free(command_buffer);
    DBUG_VOID_RETURN;
  }

  inline bool is_connected()
  {
    return clustrix_net.net.vio;
  }

  int connect();

  void disconnect(bool is_destructor = FALSE);
  int begin_trans();
  int commit_trans();
  int rollback_trans();
  inline bool has_trans()
  {
    return has_transaction;
  }

  int begin_stmt_trans();
  int commit_stmt_trans();
  int rollback_stmt_trans();
  inline bool has_stmt_trans()
  {
    return has_statement_trans;
  }

  int run_query(String &stmt);

  int write_row(ulonglong clustrix_table_oid,
                uchar *packed_row, size_t packed_size);
  int key_update(ulonglong clustrix_table_oid,
                 uchar *packed_key, size_t packed_key_length,
                 MY_BITMAP *update_set,
                 uchar *packed_new_data, size_t packed_new_length);
  int key_delete(ulonglong clustrix_table_oid,
                 uchar *packed_key, size_t packed_key_length);
  int key_read(ulonglong clustrix_table_oid, uint index, MY_BITMAP *read_set,
               uchar *packed_key, ulong packed_key_length,
               uchar **rowdata, ulong *rowdata_length);

  enum sort_order {SORT_NONE = 0, SORT_ASC = 1, SORT_DESC = 2};
  enum scan_type {
    READ_KEY_OR_NEXT,  /* rows with key and greater */
    READ_KEY_OR_PREV,  /* rows with key and less. */
    READ_AFTER_KEY,    /* rows with keys greater than key */
    READ_BEFORE_KEY,   /* rows with keys less than key */
    READ_FROM_START,   /* rows with forwards from first key. */
    READ_FROM_LAST,    /* rows with backwards from last key. */
  };

  int scan_table(ulonglong clustrix_table_oid, uint index,
                 enum sort_order sort, MY_BITMAP *read_set,
                 ulonglong *scan_refid);
  int scan_next(ulonglong scan_refid, uchar **rowdata, ulong *rowdata_length);
  int scan_end(ulonglong scan_refid);
  int scan_query(String &stmt, uchar *fieldtype, uint fields, uchar *null_bits,
                 uint null_bits_size, uchar *field_metadata,
                 uint field_metadata_size, ulonglong *scan_refid);
  int scan_from_key(ulonglong clustrix_table_oid, uint index, 
                    enum scan_type scan_dir, bool sorted_scan,
                    MY_BITMAP *read_set, uchar *packed_key,
                    ulong packed_key_length, ulonglong *scan_refid);

  int populate_table_list(LEX_CSTRING *db, handlerton::discovered_list *result);
  int discover_table_details(LEX_CSTRING *db, LEX_CSTRING *name, THD *thd,
                             TABLE_SHARE *share);
private:
  int expand_command_buffer(size_t add_length);
  int add_command_operand_uchar(uchar value);
  int add_command_operand_uint(uint value);
  int add_command_operand_ulonglong(ulonglong value);
  int add_command_operand_lcb(ulonglong value);
  int add_command_operand_str(const uchar *str, size_t length);
  int add_command_operand_vlstr(const uchar *str, size_t length);
  int add_command_operand_lex_string(LEX_CSTRING str);
  int add_command_operand_bitmap(MY_BITMAP *bitmap);
  int send_command();
  int read_query_response();
};
#endif  // _clustrix_connection_h
