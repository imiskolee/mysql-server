#ifndef ITEM_JSON_FUNC_INCLUDED
#define ITEM_JSON_FUNC_INCLUDED

/* Copyright (c) 2015, 2016, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "my_global.h"
#include "json_dom.h"           // Json_dom, Json_wrapper
#include "json_path.h"          // Json_path
#include "item_strfunc.h"       // Item_str_func
#include "mem_root_array.h"     // Mem_root_array
#include "prealloced_array.h"   // Prealloced_array
#include "sql_alloc.h"          // Sql_alloc
#include <type_traits>          // is_base_of

class Item_func_like;
class Json_scalar_holder;

/** For use by JSON_CONTAINS_PATH() and JSON_SEARCH() */
enum enum_one_or_all_type
{
  ooa_one,
  ooa_all,
  ooa_null,
  ooa_error,
  ooa_uninitialized
};

/**
  Path cache for JSON functions. Caches parsed path
  objects for arguments which are string literals.
  Maintains a vector of path objects and an array of
  ints which map path argument numbers to slots in
  the array.
*/
class Json_path_cache
{
private:
  // holder for path strings
  String m_path_value;

  // list of paths
  Prealloced_array<Json_path, 8, false> m_paths;

  // map argument indexes to indexes into m_paths
  Mem_root_array<int, true> m_arg_idx_to_vector_idx;

  // remembers whether a constant path was null or invalid
  Mem_root_array<bool, true> m_arg_idx_to_problem_indicator;

  // number of cells in m_arg_idx_to_vector
  uint m_size;

public:
  Json_path_cache(THD *thd, uint size);
  ~Json_path_cache();

  /**
    Parse a path expression if necessary. Does nothing if the path
    expression is constant and it has already been parsed. Assumes that
    we've already verified that the path expression is not null. Raises an
    error if the path expression is syntactically incorrect. Raises an
    error if the path expression contains wildcard tokens but is not
    supposed to. Otherwise puts the parsed path onto the
    path vector.

    @param[in]  args             Array of args to a JSON function
    @param[in]  arg_idx          Index of the path_expression in args
    @param[in]  forbid_wildcards True if the path shouldn't contain * or **

    @returns false on success, true on error or if the path is NULL
  */
  bool parse_and_cache_path(Item ** args, uint arg_idx,
                            bool forbid_wildcards);


  /**
    Return an already parsed path expression.

    @param[in]  arg_idx   Index of the path_expression in the JSON function args

    @returns the already parsed path
  */
  Json_path *get_path(uint arg_idx);

  /**
    Reset the cache for re-use when a statement is re-executed.
  */
  void reset_cache();
};

/* JSON function support  */

/**
  Base class for all item functions that a return JSON value
*/
class Item_json_func : public Item_func
{
protected:
  /// String used when reading JSON binary values or JSON text values.
  String m_value;
  /// String used for converting JSON text values to utf8mb4 charset.
  String m_conversion_buffer;
  /// String used for converting a JSON value to text in val_str().
  String m_string_buffer;

  // Cache for constant path expressions
  Json_path_cache m_path_cache;

  type_conversion_status save_in_field_inner(Field *field, bool no_conversions);

public:
  Item_json_func(THD *thd, const POS &pos, Item *a) : Item_func(pos, a),
    m_path_cache(thd, 1)
  {}
  Item_json_func(THD *thd, const POS &pos, Item *a, Item *b) : Item_func(pos, a, b),
    m_path_cache(thd, 2)
  {}
  Item_json_func(THD *thd, const POS &pos, Item *a, Item *b, Item *c)
    : Item_func(pos, a, b, c), m_path_cache(thd, 3)
  {}
  Item_json_func(THD *thd, const POS &pos, PT_item_list *a) : Item_func(pos, a),
    m_path_cache(thd, arg_count)
  {}

  enum_field_types field_type() const { return MYSQL_TYPE_JSON; }

  virtual bool resolve_type(THD *thd)
  {
    max_length= MAX_BLOB_WIDTH;
    maybe_null= true;
    collation.set(&my_charset_utf8mb4_bin, DERIVATION_IMPLICIT);
    return false;
  }
  enum Item_result result_type () const { return STRING_RESULT; }
  String *val_str(String *arg);
  bool get_date(MYSQL_TIME *ltime, my_time_flags_t fuzzydate);
  bool get_time(MYSQL_TIME *ltime);
  longlong val_int();
  double val_real();
  my_decimal *val_decimal(my_decimal *decimal_value);

  /** Cleanup between executions of the statement */
  void cleanup();

  Item_result cast_to_int_type () const { return INT_RESULT; }
};

/**
  Return the JSON value of the argument in a wrapper. Abstracts whether
  the value comes from a field or a function. Does not handle literals.
  See also get_json_wrapper.

  @param[in]     args     the arguments
  @param[in]     arg_idx  the argument index
  @param[in,out] result   the JSON value wrapper

  @returns false iff the argument is a JSON field or function result
*/
bool json_value(Item **args, uint arg_idx, Json_wrapper *result);

/**
  Return the JSON value of the argument in a wrapper. Abstracts whether
  the value comes from a field or a function or a valid JSON text.

  @param[in]  args          the arguments
  @param[in]  arg_idx       the argument index
  @param[out] str           the string buffer
  @param[in]  func_name     the name of the function we are executing
  @param[out] wrapper       the JSON value wrapper
  @param[in]  preserve_neg_zero_int
                            Whether integer negative zero should be preserved.
                            If set to TRUE, -0 is handled as a DOUBLE. Double
                            negative zero (-0.0) is preserved regardless of what
                            this parameter is set to.
  @returns false if we found a value or NULL, true if not.
*/
bool get_json_wrapper(Item **args, uint arg_idx, String *str,
                      const char *func_name, Json_wrapper *wrapper,
                      bool preserve_neg_zero_int= false);

/**
  Convert Json values or MySQL values to JSON.

  @param[in]     args       arguments to function
  @param[in]     arg_idx    the index of the argument to process
  @param[in]     calling_function    name of the calling function
  @param[in,out] value      working area (if the returned Json_wrapper points
                            to a binary value rather than a DOM, this string
                            will end up holding the binary representation, and
                            it must stay alive until the wrapper is destroyed
                            or converted from binary to DOM)
  @param[in,out] tmp        temporary scratch space for converting strings to
                            the correct charset; only used if accept_string is
                            true and conversion is needed
  @param[in,out] wr         the result wrapper
  @param[in,out] scalar     pointer to pre-allocated memory that can be
                            borrowed by the result wrapper if the result is a
                            scalar. If the pointer is NULL, memory for a
                            scalar result will be allocated on the heap.
  @param[in]                accept_string
                            if true, accept MySQL strings as JSON strings
                            by converting them to UTF8, else emit an error
  @returns false if we found a value or NULL, true otherwise
*/
bool get_json_atom_wrapper(Item **args, uint arg_idx,
                           const char *calling_function, String *value,
                           String *tmp, Json_wrapper *wr,
                           Json_scalar_holder *scalar, bool accept_string);

/**
  Check a non-empty val for character set. If it has character set
  my_charset_binary, signal error and return false. Else, try to convert to
  my_charset_utf8mb4_binary. If this fails, signal error and return true, else
  return false.

  @param[in]     val       the string to be checked
  @param[in,out] buf       buffer to hold the converted string
  @param[out]    resptr    the resulting, possibly converted string,
                           only set if no error
  @param[out]    reslength the length of resptr
  @param[in]     require_string
                           If true, give error messages if binary string. If we
                           see a conversion error (space), we give error
                           notwithstanding this parameter value

  @returns True if the string could not be converted. False on success.
*/
bool ensure_utf8mb4(String *val,
                    String *buf,
                    const char **resptr,
                    size_t *reslength,
                    bool require_string);

/**
  Represents the JSON function JSON_VALID( <value> )
*/
class Item_func_json_valid :public Item_int_func
{
  String m_value;
public:
  Item_func_json_valid(const POS &pos, Item *a) : Item_int_func(pos, a)
  {}

  const char *func_name() const
  {
    return "json_valid";
  }

  bool is_bool_func()
  {
    return 1;
  }

  longlong val_int();

  virtual bool resolve_type(THD *thd)
  {
    maybe_null= true;
    return false;
  }
};

/**
  Represents the JSON function JSON_CONTAINS()
*/
class Item_func_json_contains :public Item_int_func
{
  String m_doc_value;
  Json_path_cache m_path_cache;

 public:
  Item_func_json_contains(THD *thd, const POS &pos, PT_item_list *a)
    : Item_int_func(pos, a), m_path_cache(thd, arg_count)
  {}

  const char *func_name() const
  {
    return "json_contains";
  }

  bool is_bool_func()
  {
    return 1;
  }

  longlong val_int();

  virtual bool resolve_type(THD *thd)
  {
    maybe_null= true;
    return false;
  }

  /** Cleanup between executions of the statement */
  void cleanup();
};

/**
  Represents the JSON function JSON_CONTAINS_PATH()
*/
class Item_func_json_contains_path :public Item_int_func
{
  String m_doc_value;
  String m_one_or_all_value;
  enum_one_or_all_type m_cached_ooa;

  // Cache for constant path expressions
  Json_path_cache m_path_cache;

public:
  Item_func_json_contains_path(THD *thd, const POS &pos, PT_item_list *a)
    : Item_int_func(pos, a),
    m_cached_ooa(ooa_uninitialized), m_path_cache(thd, arg_count)
  {}

  const char *func_name() const
  {
    return "json_contains_path";
  }

  bool is_bool_func()
  {
    return 1;
  }

  longlong val_int();

  virtual bool resolve_type(THD *thd)
  {
    maybe_null= true;
    return false;
  }

  /** Cleanup between executions of the statement */
  void cleanup();
};

/**
  Represents the JSON function JSON_TYPE
*/
class Item_func_json_type :public Item_str_func
{
  String m_value;
public:
  Item_func_json_type(const POS &pos, Item *a) : Item_str_func(pos, a)
  {}

  const char *func_name() const
  {
    return "json_type";
  }

  virtual bool resolve_type(THD *thd)
  {
    maybe_null= true;
    m_value.set_charset(&my_charset_utf8mb4_bin);
    fix_length_and_charset(Json_dom::typelit_max_length,
                           &my_charset_utf8mb4_bin);
    return false;
  };

  String *val_str(String *);
};

/**
  Represents a "CAST( <value> AS JSON )" coercion.
*/
class Item_json_typecast :public Item_json_func
{
public:
  Item_json_typecast(THD *thd, const POS &pos, Item *a) : Item_json_func(thd, pos, a)
  {}

  void print(String *str, enum_query_type query_type);
  const char *func_name() const { return "cast_as_json"; }
  const char *cast_type() const { return "json"; }
  bool val_json(Json_wrapper *wr);
};

/**
  Represents the JSON function JSON_LENGTH()
*/
class Item_func_json_length :public Item_int_func
{
  String m_doc_value;

  // Cache for constant path expressions
  Json_path_cache m_path_cache;

public:
  Item_func_json_length(THD *thd, const POS &pos, Item *a)
    : Item_int_func(pos, a), m_path_cache(thd, 1)
  {}

  Item_func_json_length(THD *thd, const POS &pos, Item *a, Item *b)
    : Item_int_func(pos, a, b), m_path_cache(thd, 2)
  {}

  virtual bool resolve_type(THD *thd)
  {
    maybe_null= true;
    return false;
  }

  const char *func_name() const
  {
    return "json_length";
  }

  longlong val_int();

  /** Cleanup between executions of the statement */
  void cleanup();
};

/**
  Represents the JSON function JSON_DEPTH()
*/
class Item_func_json_depth :public Item_int_func
{
  String m_doc_value;

public:
  Item_func_json_depth(const POS &pos, Item *a)
    : Item_int_func(pos, a)
  {}

  const char *func_name() const
  {
    return "json_depth";
  }

  longlong val_int();
};

/**
  Represents the JSON function JSON_KEYS()
*/
class Item_func_json_keys :public Item_json_func
{
  String m_doc_value;

public:
  Item_func_json_keys(THD *thd, const POS &pos, Item *a)
    : Item_json_func(thd, pos, a)
  {}

  Item_func_json_keys(THD *thd, const POS &pos, Item *a, Item *b)
    : Item_json_func(thd, pos, a, b)
  {}

  const char *func_name() const
  {
    return "json_keys";
  }

  bool val_json(Json_wrapper *wr);
};

/**
  Represents the JSON function JSON_EXTRACT()
*/
class Item_func_json_extract :public Item_json_func
{
  String m_doc_value;

public:
  Item_func_json_extract(THD *thd, const POS &pos, PT_item_list *a)
    : Item_json_func(thd, pos, a)
  {}

  Item_func_json_extract(THD *thd, const POS &pos, Item *a, Item *b)
    : Item_json_func(thd, pos, a, b)
  {}

  const char *func_name() const
  {
    return "json_extract";
  }

  bool val_json(Json_wrapper *wr);
};

/**
  Represents the JSON function JSON_ARRAY_APPEND()
*/
class Item_func_json_array_append :public Item_json_func
{
  String m_doc_value;

public:
  Item_func_json_array_append(THD *thd, const POS &pos, PT_item_list *a)
    : Item_json_func(thd, pos, a)
  {}

  const char *func_name() const
  {
    return "json_array_append";
  }

  bool val_json(Json_wrapper *wr);
};

/**
  Represents the JSON function JSON_INSERT()
*/
class Item_func_json_insert :public Item_json_func
{
  String m_doc_value;
  Json_path_clone m_path;

public:
  Item_func_json_insert(THD *thd, const POS &pos, PT_item_list *a)
    : Item_json_func(thd, pos, a)
  {}

  const char *func_name() const
  {
    return "json_insert";
  }

  bool val_json(Json_wrapper *wr);
};

/**
  Represents the JSON function JSON_ARRAY_INSERT()
*/
class Item_func_json_array_insert :public Item_json_func
{
  String m_doc_value;
  Json_path_clone m_path;

public:
  Item_func_json_array_insert(THD *thd, const POS &pos, PT_item_list *a)
    : Item_json_func(thd, pos, a)
  {}

  const char *func_name() const
  {
    return "json_array_insert";
  }

  bool val_json(Json_wrapper *wr);
};

/**
  Common base class for JSON_SET() and JSON_REPLACE().
*/
class Item_func_json_set_replace :public Item_json_func
{
  /// True if this is JSON_SET, false if it is JSON_REPLACE.
  const bool m_json_set;
  String m_doc_value;
  Json_path_clone m_path;

protected:
  Item_func_json_set_replace(THD *thd, const POS &pos, PT_item_list *a, bool json_set)
    : Item_json_func(thd, pos, a), m_json_set(json_set)
  {}

public:
  bool val_json(Json_wrapper *wr);
};

/**
  Represents the JSON function JSON_SET()
*/
class Item_func_json_set :public Item_func_json_set_replace
{
public:
  Item_func_json_set(THD *thd, const POS &pos, PT_item_list *a)
    : Item_func_json_set_replace(thd, pos, a, true)
  {}

  const char *func_name() const
  {
    return "json_set";
  }
};

/**
  Represents the JSON function JSON_REPLACE()
*/
class Item_func_json_replace :public Item_func_json_set_replace
{
public:
  Item_func_json_replace(THD *thd, const POS &pos, PT_item_list *a)
    : Item_func_json_set_replace(thd, pos, a, false)
  {}

  const char *func_name() const
  {
    return "json_replace";
  }
};

/**
  Represents the JSON function JSON_ARRAY()
*/
class Item_func_json_array :public Item_json_func
{
public:
  Item_func_json_array(THD *thd, const POS &pos, PT_item_list *a)
    : Item_json_func(thd, pos, a)
  {}

  const char *func_name() const
  {
    return "json_array";
  }

  bool val_json(Json_wrapper *wr);
};

/**
  Represents the JSON function JSON_OBJECT()
*/
class Item_func_json_row_object :public Item_json_func
{
  String tmp_key_value;
public:
  Item_func_json_row_object(THD *thd, const POS &pos, PT_item_list *a)
    : Item_json_func(thd, pos, a)
  {}

  const char *func_name() const
  {
    return "json_object";
  }

  bool val_json(Json_wrapper *wr);
};

/**
  Represents the JSON function JSON_SEARCH()
*/
class Item_func_json_search :public Item_json_func
{
  String m_doc_value;
  String m_one_or_all_value;
  String m_search_string_value;
  enum_one_or_all_type m_cached_ooa;
  String m_escape;

  // LIKE machinery
  Item_string *m_source_string_item;
  Item_func_like *m_like_node;
public:
  /**
   Construct a JSON_SEARCH() node.

   @param     thd Current session.
   @param[in] pos Parser position
   @param[in] a   Nodes which must be fixed (i.e. bound/resolved)

   @returns a JSON_SEARCH() node.
  */
  Item_func_json_search(THD *thd, const POS &pos, PT_item_list *a)
    : Item_json_func(thd, pos, a),
    m_cached_ooa(ooa_uninitialized)
  {}


  const char *func_name() const
  {
    return "json_search";
  }

  bool val_json(Json_wrapper *wr);

  /**
    Bind logic for the JSON_SEARCH() node.
  */
  bool fix_fields(THD *, Item **);

  /** Cleanup between executions of the statement */
  void cleanup();
};

/**
  Represents the JSON function JSON_REMOVE()
*/
class Item_func_json_remove :public Item_json_func
{
  String m_doc_value;

public:
  Item_func_json_remove(THD *thd, const POS &pos, PT_item_list *a);

  const char *func_name() const
  {
    return "json_remove";
  }

  bool val_json(Json_wrapper *wr);
};

/**
  Represents the JSON function JSON_MERGE()
*/
class Item_func_json_merge :public Item_json_func
{
public:
  Item_func_json_merge(THD *thd, const POS &pos, PT_item_list *a)
    : Item_json_func(thd, pos, a)
  {}

  const char *func_name() const
  {
    return "json_merge";
  }

  bool val_json(Json_wrapper *wr);
};

/**
  Represents the JSON function JSON_QUOTE()
*/
class Item_func_json_quote :public Item_str_func
{
  String m_value;
public:
  Item_func_json_quote(const POS &pos, PT_item_list *a)
    : Item_str_func(pos, a)
  {}

  const char *func_name() const
  {
    return "json_quote";
  }

  virtual bool resolve_type(THD *thd)
  {
    maybe_null= true;

    /*
     Any interior character could be replaced by a 6 character
     escape sequence. Plus we will add 2 framing quote characters.
    */
    uint32 max_char_length= (6 * args[0]->max_length) + 2;
    fix_length_and_charset(max_char_length, &my_charset_utf8mb4_bin);
    return false;
  };

  String *val_str(String *tmpspace);
};

/**
  Represents the JSON function JSON_UNQUOTE()
*/
class Item_func_json_unquote :public Item_str_func
{
  String m_value;
public:
  Item_func_json_unquote(const POS &pos, PT_item_list *a)
    : Item_str_func(pos, a)
  {}

  const char *func_name() const
  {
    return "json_unquote";
  }

  virtual bool resolve_type(THD *thd)
  {
    maybe_null= true;
    fix_length_and_charset(args[0]->max_length, &my_charset_utf8mb4_bin);
    return false;
  };

  String *val_str(String *str);
};

/**
  Turn a GEOMETRY value into a JSON value per the GeoJSON specification revison 1.0.
  This method is implemented in item_geofunc.cc.

  @param[in,out] wr The wrapper to be stuffed with the JSON value.
  @param[in]     geometry_arg The source GEOMETRY value.
  @param[in]     calling_function Name of user-invoked function (for errors)
  @param[in]     max_decimal_digits See the user documentation for ST_AsGeoJSON.
  @param[in]     add_bounding_box See the user documentation for ST_AsGeoJSON.
  @param[in]     add_short_crs_urn See the user documentation for ST_AsGeoJSON.
  @param[in]     add_long_crs_urn See the user documentation for ST_AsGeoJSON.
  @param[in,out] geometry_srid Spatial Reference System Identifier to be filled in.

  @return false if the conversion succeeds, true otherwise
*/
bool geometry_to_json(Json_wrapper *wr, Item *geometry_arg,
                      const char *calling_function,
                      int max_decimal_digits,
                      bool add_bounding_box,
                      bool add_short_crs_urn,
                      bool add_long_crs_urn,
                      uint32 *geometry_srid);

/**
  A class that is capable of holding objects of any sub-type of
  Json_scalar. Used for pre-allocating space in query-duration memory
  for JSON scalars that are to be returned by get_json_atom_wrapper().
*/
class Json_scalar_holder : public Sql_alloc
{
  /**
    Union of all concrete subclasses of Json_scalar. The union is
    never instantiated. It is only used for finding how much space
    needs to be allocated for #m_buffer.
  */
  union Any_json_scalar
  {
    Json_string m_string;
    Json_decimal m_decimal;
    Json_int m_int;
    Json_uint m_uint;
    Json_double m_double;
    Json_boolean m_boolean;
    Json_null m_null;
    Json_datetime m_datetime;
    Json_opaque m_opaque;
    // Need explicitly deleted destructor to silence warning on MSVC.
    ~Any_json_scalar() = delete;
  };

  /// The buffer in which the Json_scalar value is stored.
  char m_buffer[sizeof(Any_json_scalar)];

  /// True if and only if a value has been assigned to the holder.
  bool m_assigned= false;

  /// Clear the holder, and destroy the held value if there is one.
  void clear()
  {
    if (m_assigned)
    {
      get()->~Json_scalar();
      m_assigned= false;
    }
  }
public:
  /// Destructor. The held value is destroyed, if there is one.
  ~Json_scalar_holder() { clear(); }

  /// Get a pointer to the held object, or nullptr if there is none.
  Json_scalar *get()
  {
    return m_assigned ? pointer_cast<Json_scalar *>(&m_buffer) : nullptr;
  }

  /**
    Construct a new Json_scalar value in this Json_scalar_holder.
    If a value is already held, the old value is destroyed and replaced.
    @tparam T which type of Json_scalar to create
    @param args the arguments to T's constructor
  */
  template <typename T, typename... Args> void emplace(Args&&... args)
  {
    static_assert(std::is_base_of<Json_scalar, T>::value, "Not a Json_scalar");
    static_assert(sizeof(T) <= sizeof(m_buffer), "Buffer is too small");
    clear();
    new (&m_buffer) T(std::forward<Args>(args)...);
    m_assigned= true;
  }
};

#endif /* ITEM_JSON_FUNC_INCLUDED */
