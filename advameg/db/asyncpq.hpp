//
//  asyncpq.hpp
//  wostok
//
//  Created by Łukasz Gwiżdż on 28/03/16.
//  Copyright © 2016 Łukasz Gwiżdż. All rights reserved.
//

#ifndef asyncpq_hpp
#define asyncpq_hpp

#include "libpq-fe.h"
#include <boost/asio.hpp>
#include <experimental/string_view>
#include <type_traits>
#include <functional>
#include <mutex>
#include <limits>
#include <condition_variable>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/scope_exit.hpp>

namespace advameg {
namespace db {
namespace postgres {

enum class error
{
    connect_error = 1,
    general_error,
};

namespace detail {
class pq_category : public boost::system::error_category
{
public:
    const char* name() const noexcept final override
    {
        return "advameg.postgresql";
    }

    std::string message(int eval) const final override
    {
        switch (static_cast<error>(eval))
        {
            case error::connect_error:
            case error::general_error:
                return msg.load();
            default:
                return "";
        }
    }

    std::atomic<const char*> msg{nullptr};
};
} // namespace detail

inline const boost::system::error_category& get_pq_category(bool newmsg, const char* errmsg)
{
    static detail::pq_category instance;
    if (newmsg)
        instance.msg = errmsg;
    return instance;
}

inline boost::system::error_code make_error_code
(
 error e,
 bool newmsg = false,
 const char* msg = nullptr
) noexcept
{
    return boost::system::error_code{static_cast<int>(e), get_pq_category(newmsg, msg)};
}

namespace detail {
template <class T>
T to_integer(const char* buf)
{
    long long t = 0;
    int n = 0;
    auto const converted = std::sscanf(buf, "%lld%n", &t, &n);
    if (converted == 1 && std::strlen(buf) == static_cast<std::size_t>(n))
    {
        if (t <= static_cast<long long>(std::numeric_limits<T>::max()) &&
            t >= static_cast<long long>(std::numeric_limits<T>::min()))
        {
            return static_cast<T>(t);
        }
        else {
            BOOST_THROW_EXCEPTION(std::out_of_range{"Cannot convert data"});
        }
    }
    else
    {
        if (buf[0] == 't' && buf[1] == '\0')
            return static_cast<T>(true);
        else if (buf[0] == 'f' && buf[1] == '\0')
            return static_cast<T>(false);
        else
            BOOST_THROW_EXCEPTION(std::out_of_range{"Cannot convert data"});
    }
}

inline double to_double(const char* buf)
{
    double t{};
    int n{};
    auto const converted = std::sscanf(buf, "%lf%n", &t, &n);
    if (converted == 1 && static_cast<std::size_t>(n) == std::strlen(buf))
        return t;
    else
        BOOST_THROW_EXCEPTION(std::out_of_range{"Cannot convert data"});
}

inline std::tm to_tm(const char* buf)
{
    auto parse10 = [](const char*& p1, char*& p2) {
        auto v = std::strtol(p1, &p2, 10);
        if (p2 != p1) {
            p1 = p2 + 1;
            return v;
        }
        else
            BOOST_THROW_EXCEPTION(std::runtime_error{"Cannot convert data to std::tm."});
    };

    const char* p1 = buf;
    char* p2;
    auto a = parse10(p1, p2);
    auto separator = *p2;
    auto b = parse10(p1, p2);
    auto c = parse10(p1, p2);

    long year = 1900, month = 1, day = 1,
    hour = 0, minute = 0, second = 0;

    if (*p2 == ' ') {
        year = a; month = b; day = c;
        hour = parse10(p1, p2);
        minute = parse10(p1, p2);
        second = parse10(p1, p2);
    }
    else {
        if (separator == '-') {
            year = a; month = b; day = c;
        }
        else {
            hour = a; minute = b; second = c;
        }
    }

    std::tm t{};
    t.tm_isdst = -1;
    t.tm_year = int(year - 1900);
    t.tm_mon = int(month - 1);
    t.tm_mday = int(day);
    t.tm_hour = int(hour);
    t.tm_min = int(minute);
    t.tm_sec = int(second);
    std::mktime(&t);
    return t;
}

struct data_extractor
{
    virtual ~data_extractor() = default;
    virtual void parse_raw_data(const char* buf, int nlen) = 0;
};

template <class T>
struct data_extractor_impl : data_extractor
{
    virtual void parse_raw_data(const char*, int) override final;
    T value;
};

template <>
inline void data_extractor_impl<int>::parse_raw_data(const char* buf, int) {
    value = to_integer<int>(buf);
}

template <>
inline void data_extractor_impl<long long>::parse_raw_data(const char* buf, int) {
    value = to_integer<long long>(buf);
}

template <>
inline void data_extractor_impl<std::string>::parse_raw_data(const char* buf, int nlen) {
    value = std::string(buf, nlen);
}

template <>
inline void data_extractor_impl<double>::parse_raw_data(const char* buf, int) {
    value = to_double(buf);
}

template <>
inline void data_extractor_impl<std::tm>::parse_raw_data(const char* buf, int) {
    value = to_tm(buf);
}

template <class T>
struct is_std_vector : std::false_type {};

template <class T, class Alloc>
struct is_std_vector<std::vector<T, Alloc>> : std::true_type
{};

template <class T>
struct is_std_array : std::false_type {};

template <class T, std::size_t N>
struct is_std_array<std::array<T, N>> : std::true_type {};

template <class T>
using is_stl_array = std::integral_constant<bool, is_std_array<T>::value || is_std_vector<T>::value>;

template <class T>
using is_basic_singular = std::integral_constant<bool, std::is_arithmetic<T>::value ||
    std::is_same<T, std::string>::value || std::is_same<T, std::tm>::value>;

template <class T>
using accepted_by_int_extractor =
 std::integral_constant<bool, std::is_integral<T>::value && (sizeof(T) <= sizeof(int)) && std::is_convertible<T, int>::value>;

template <class T>
using accepted_by_long_long_extractor = std::integral_constant<bool, std::is_same<T, long long>::value ||
 (std::is_integral<T>::value && ((sizeof(T) <= sizeof(long long)) && (sizeof(T) > sizeof(int)))
 && std::is_convertible<T, long long>::value)>;

template <class T>
using accepted_by_float_extractor = std::integral_constant<bool, std::is_same<T, double>::value ||
 std::is_same<T, float>::value || (std::is_floating_point<T>::value && std::is_convertible<T, long double>::value)>;

template <class T>
using accepted_by_string_extractor = std::integral_constant<bool, std::is_same<T, std::string>::value ||
    is_stl_array<T>::value>;

template <class T>
using accepted_by_date_time_extractor = std::integral_constant<bool, std::is_same<T, std::tm>::value>;

template <class T, class = void>
struct extractor_traits {
    static_assert(!sizeof(T), "Provided type is not supported by ORM.");
};

template <class T>
struct extractor_traits<T, typename std::enable_if<accepted_by_float_extractor<T>::value>::type>
{
    using type = detail::data_extractor_impl<double>;
};

template <class T>
struct extractor_traits<T, typename std::enable_if<accepted_by_string_extractor<T>::value>::type>
{
    using type = detail::data_extractor_impl<std::string>;
};

template <class T>
struct extractor_traits<T, typename std::enable_if<accepted_by_int_extractor<T>::value>::type>
{
    using type = detail::data_extractor_impl<int>;
};

template <class T>
struct extractor_traits<T, typename std::enable_if<accepted_by_long_long_extractor<T>::value>::type>
{
    using type = detail::data_extractor_impl<long long>;
};

template <class T>
struct extractor_traits<T, typename std::enable_if<accepted_by_date_time_extractor<T>::value>::type>
{
    using type = detail::data_extractor_impl<std::tm>;
};

template <class T>
typename std::enable_if<detail::is_basic_singular<T>::value>::type extract_value(data_extractor* pe, T& output)
{
    BOOST_ASSERT(pe);
    auto e = dynamic_cast<typename extractor_traits<T>::type*>(pe);
    if (!e)
        BOOST_THROW_EXCEPTION(std::runtime_error{"Invalid extractor type."});
    output = static_cast<T>(e->value);
}

template <class Output>
auto assure_size(std::size_t n, Output& result) -> decltype(result.resize(0))
{
    if (result.size() != n)
        result.resize(n);
}

template <class Output>
auto assure_size(std::size_t n, Output& result) -> decltype(result.fill(typename Output::value_type{}))
{
    if (result.max_size() < n)
        BOOST_THROW_EXCEPTION(std::invalid_argument{"extracted array is too large for the mapped std::array<T, N> type."});
}

template <class Output>
void extract_array(std::string const& s, Output& result)
{
    const bool has_data = !s.empty();
    if (has_data && s[0] != '{')
    {
        if (s[0] == '\\' && s[1] == 'x')
        {
            std::size_t len = 0U;
            auto buff = PQunescapeBytea(reinterpret_cast<const unsigned char*>(s.data()), &len);
            BOOST_SCOPE_EXIT_ALL(buff) {
                PQfreemem(buff);
            };
            assure_size(len, result);
            std::memcpy(result.data(), buff, len);
        }
        else {
            assure_size(s.size(), result);
            std::copy(std::begin(s), std::end(s), std::begin(result));
        }
    }
    else if (has_data)
    {
        boost::iterator_range<std::string::const_iterator> r{std::next(s.begin()), std::prev(s.end())};
        std::vector<boost::iterator_range<std::string::const_iterator>> vr;
        boost::algorithm::split(vr, r, boost::is_any_of(","), boost::algorithm::token_compress_on);

        assure_size(vr.size(), result);
        using T = typename Output::value_type;
        for (std::size_t i = 0; i < vr.size(); ++i)
        {
            result[i] = boost::lexical_cast<T>(vr[i]);
        }
    }
}

template <class T>
typename std::enable_if<is_stl_array<T>::value>::type extract_value(data_extractor* pe, T& output)
{
    BOOST_ASSERT(pe);
    auto e = dynamic_cast<typename extractor_traits<T>::type*>(pe);
    if (!e)
        BOOST_THROW_EXCEPTION(std::runtime_error{"Invalid extractor type."});
    extract_array(e->value, output);
}

} // namespace detail


struct result final
{
    explicit result(PGresult* result = nullptr) noexcept
        : res{result}
    {}

    ~result() noexcept {
        if (res)
            PQclear(res);
    }

    result(result const&) = delete;
    result& operator=(result const&) = delete;

    result(result&& other) noexcept
        : res{other.res}, nrows{other.nrows}, nfields{other.nfields}
    {
        other.res = nullptr;
    }

    result& operator=(result&& other) noexcept
    {
        res = other.res;
        nrows = other.nrows;
        nfields = other.nfields;
        other.res = nullptr;
        return *this;
    }

    operator const PGresult*() const noexcept {
        return res;
    }

    ExecStatusType status() const noexcept {
        return PQresultStatus(res);
    }

    operator bool() const noexcept {
        const auto res = status();
        return !(res == PGRES_BAD_RESPONSE ||
                 res ==  PGRES_NONFATAL_ERROR ||
                 res == PGRES_FATAL_ERROR);
    }

    auto num_rows() const noexcept {
        if (nrows == -1)
            nrows = PQntuples(res);
        return nrows;
    }

    auto num_fields() const noexcept {
        if (nfields == -1)
            nfields = PQnfields(res);
        return nfields;
    }

    auto operator[](std::size_t) = delete;

    auto begin() const noexcept
    {
        return row_const_iterator{*this};
    }

    auto end() const noexcept
    {
        return row_const_iterator{*this, num_rows()};
    }

    auto cbegin() const noexcept
    {
        return begin();
    }

    auto cend() const noexcept
    {
        return end();
    }

    enum class data_type
    {
        string_,
        date_,
        double_,
        integer_,
        long_long_
    };

    class row_const_iterator;
    class row final
    {
        friend class row_const_iterator;
        result const* res;
        int row_nbr;

        int map_field_name_to_pos(std::experimental::string_view field_name) const
        {
            BOOST_ASSERT(res != nullptr);
            BOOST_ASSERT(res->column_description);
            if (auto pdesc = res->column_description.by_name(field_name))
                return pdesc->num();
            return -1;
        }

        static std::unique_ptr<detail::data_extractor> make_extractor(data_type t)
        {
            switch (t)
            {
                case data_type::string_:
                    return std::make_unique<detail::data_extractor_impl<std::string>>();
                case data_type::date_:
                    return std::make_unique<detail::data_extractor_impl<std::tm>>();
                case data_type::double_:
                    return std::make_unique<detail::data_extractor_impl<double>>();
                case data_type::integer_:
                    return std::make_unique<detail::data_extractor_impl<int>>();
                case data_type::long_long_:
                    return std::make_unique<detail::data_extractor_impl<long long>>();
                default:
                    BOOST_ASSERT(0);
                    return nullptr;
            }
        }

        static auto get_extractor(data_type t)
        {
            static std::array<std::unique_ptr<detail::data_extractor>, 5> extractors;

            auto& e = extractors[static_cast<int>(t)];
            if (nullptr == e) {
                e = make_extractor(t);
                return e.get();
            } else {
                return e.get();
            }
        }

    public:

        explicit row(result const* const r = nullptr, int nrow = -1) noexcept
            : res{r}, row_nbr{nrow}
        {}

        auto size() const noexcept {
            return res->num_fields();
        }

        template <class MappedType>
        std::optional<MappedType> get(std::experimental::string_view field_name) const
        {
            return get<MappedType>(map_field_name_to_pos(field_name));
        }

        template <class MappedType>
        std::optional<MappedType> get(int field_nbr) const
        {
            // TO DO: handle field_nbr == -1, and wrong extractor cases, exceptions or error_codes
            BOOST_ASSERT(res != nullptr);
            BOOST_ASSERT(res->column_description);
            if (PQgetisnull(*res, row_nbr, field_nbr) != 0)
                return {};
            const auto buf = PQgetvalue(*res, row_nbr, field_nbr);
            const auto nlen = PQgetlength(*res, row_nbr, field_nbr);
            auto const& desc = res->column_description[field_nbr];
            MappedType val{};
            auto extractor = get_extractor(desc.type());
            extractor->parse_raw_data(buf, nlen);
            detail::extract_value<MappedType>(extractor, val);
            return val;
        }

        template <class MappedStruct>
        void operator>>(MappedStruct& d) const
        {
            map_onto(*this, d);
        }
    };

    mutable class column_description
    {
        class column_descr_t : std::tuple<int, std::string, data_type>
        {
        public:
            using std::tuple<int, std::string, data_type>::tuple;
            int num() const {
                return std::get<0>(*this);
            }

            std::string const& name() const {
                return std::get<1>(*this);
            }

            data_type type() const {
                return std::get<2>(*this);
            }
        };
        std::vector<column_descr_t> column_descr;
    public:
        void init(result const& res)
        {
            const auto nfields = res.num_fields();
            for (int pos = 0; pos < nfields; ++pos)
            {
                data_type type;
                auto const type_oid = PQftype(res, pos);
                switch (type_oid)
                {
                        // Note: the following list of OIDs was taken from the pg_type table
                        // we do not claim that this list is exchaustive or even correct.

                        // from pg_type:

                    case 25:   // text
                    case 1043: // varchar
                    case 2275: // cstring
                    case 18:   // char
                    case 1042: // bpchar
                    case 142: // xml
                    case 114:  // json
                    case 17: // bytea
                        type = data_type::string_;
                        break;

                    case 702:  // abstime
                    case 703:  // reltime
                    case 1082: // date
                    case 1083: // time
                    case 1114: // timestamp
                    case 1184: // timestamptz
                    case 1266: // timetz
                        type = data_type::date_;
                        break;

                    case 700:  // float4
                    case 701:  // float8
                    case 1700: // numeric
                        type = data_type::double_;
                        break;

                    case 16:   // bool
                    case 21:   // int2
                    case 23:   // int4
                    case 26:   // oid
                        type = data_type::integer_;
                        break;

                    case 20:   // int8
                        type = data_type::long_long_;
                        break;

                    default:
                    {
                        int form = PQfformat(res, pos);
                        int size = PQfsize(res, pos);
                        if (form == 0 && size == -1)
                        {
                            type = data_type::string_;
                        }
                        else
                        {
                            std::stringstream message;
                            message << "unknown data type with typelem: " << type_oid << " for colNum: " << pos << " with name: " << PQfname(res, pos);
                            BOOST_THROW_EXCEPTION(std::runtime_error{message.str()});
                        }
                    }
                }

                auto name = PQfname(res, pos);
                column_descr.emplace_back(pos, name, type);
            }
        }

        operator bool() const noexcept {
            return !column_descr.empty();
        }

        column_descr_t const& operator[](int i) const {
            return column_descr[i];
        }

        column_descr_t const* by_name(std::experimental::string_view name) const
        {
            auto end_ = std::cend(column_descr);
            auto pos = std::find_if(std::cbegin(column_descr), end_, [name](column_descr_t const& v) {
                return name == v.name();
            });
            if (pos == end_)
                return nullptr;
            return &(*pos);
        }

        column_descr_t const* by_num(int num) const
        {
            auto end_ = std::cend(column_descr);
            auto pos = std::find_if(std::cbegin(column_descr), end_, [num](column_descr_t const& v) {
                return num == v.num();
            });
            if (pos == end_)
                return nullptr;
            return &(*pos);
        }
    } column_description;

    class row_const_iterator final
        : public std::iterator<std::random_access_iterator_tag, row, int, row const*, row const&>
    {
        friend class result;

        explicit row_const_iterator(result const& r, int p = 0) noexcept : res{r}, pos{p}
        {
            current_row.res = &res;
            if (!r.column_description)
                r.column_description.init(r);;
        }

        result const& res;
        int pos = -1;
        mutable row current_row;

        void update_current_row() const noexcept {
            current_row.row_nbr = pos;
        }
    public:
        reference operator*() const noexcept {
            update_current_row();
            return current_row;
        }

        pointer operator->() const noexcept {
            update_current_row();
            return &current_row;
        }

        reference operator[](difference_type n) noexcept
        {
            pos = n;
            return current_row;
        }

        row_const_iterator& operator++() noexcept {
            ++pos;
            return *this;
        }

        row_const_iterator operator++(int) noexcept {
            auto t = *this; ++pos;
            return t;
        }

        row_const_iterator& operator--() noexcept {
            --pos;
            return *this;
        }

        row_const_iterator operator--(int) noexcept {
            auto t = *this; --pos;
            return t;
        }

        row_const_iterator operator+(difference_type n) const noexcept
        {
            auto t = *this;
            t.pos += n;
            return t;
        }

        row_const_iterator& operator+=(difference_type n) noexcept
        {
            pos += n;
            return *this;
        }

        row_const_iterator operator-(difference_type n) const noexcept
        {
            auto t = *this;
            t.pos -= n;
            return t;
        }

        row_const_iterator& operator-=(difference_type n) noexcept
        {
            pos -= n;
            return *this;
        }

        friend bool operator==(row_const_iterator const& a, row_const_iterator const& b) noexcept
        {
            return a.pos == b.pos;
        }

        friend bool operator!=(row_const_iterator const& a, row_const_iterator const& b) noexcept
        {
            return !(a == b);
        }

        friend difference_type operator-(row_const_iterator const& a, row_const_iterator const& b) noexcept
        {
            return a.pos - b.pos;
        }
    };

    using const_iterator = row_const_iterator;
    using iterator = const_iterator;

private:
    PGresult* res;
    mutable int nrows = -1;
    mutable int nfields = -1;
};

class session final
{
public:
    explicit session(boost::asio::io_service& io_service) : socket{io_service}
    {}

    session(session&& other) noexcept : conn{other.conn}, socket{std::move(other.socket)}
    {
        other.conn = nullptr;
    }

    session& operator=(session&& other) noexcept {
        conn = other.conn;
        other.conn = nullptr;
        socket = std::move(other.socket);
        return *this;
    }

    ~session() noexcept {
        if (conn)
            PQfinish(conn);
    }

    void open(const char* conn_str, boost::system::error_code& ec)
    {
        conn = PQconnectdb(conn_str);
        if (PQstatus(conn) != CONNECTION_OK)
        {
            ec = make_error_code(error::connect_error);
        }
        if (!PQsetnonblocking(conn, 1))
            socket.assign(boost::asio::ip::tcp::v4(), PQsocket(conn));
        else {
            ec = make_error_code(error::connect_error);
        }
    }

    template <class CompletionHandler>
    void open(const char* conn_str, CompletionHandler&& handler) noexcept
    {
        boost::asio::post([this, conn_str, h = std::forward<CompletionHandler>(handler)] {
            boost::system::error_code ec;
            this->open(conn_str, ec);
            h(ec);
        });
    }

    explicit operator bool() const noexcept
    {
        return PQstatus(conn) == CONNECTION_OK;
    }

    auto error_message() const noexcept {
        return PQerrorMessage(conn);
    }

    template <typename CompletionHandler>
    void query(const char* query, CompletionHandler&& handler) noexcept
    {
        if (0 == PQsendQuery(conn, query)) {
            std::forward<CompletionHandler>(handler)(make_error_code(), result{});
        }
        else {
            socket.async_read_some(boost::asio::null_buffers(),
               [this, handler = std::forward<CompletionHandler>(handler)]
               (boost::system::error_code, std::size_t) {
                   if (PQconsumeInput(conn) == 0) {
                       handler(make_error_code(), result{});
                   }
                   else {
                       auto res = PQgetResult(conn);
                       while (nullptr != PQgetResult(conn));
                       handler(boost::system::error_code{}, result{res});
                   }
               });
        }
    }

    auto last_error_code() const
    {
        return make_error_code();
    }
private:
    boost::system::error_code make_error_code(error e = error::general_error) const noexcept
    {
        return ::advameg::db::postgres::make_error_code(e, true, PQerrorMessage(conn));
    }

private:
    PGconn* conn{nullptr};
    boost::asio::ip::tcp::socket socket;
};

class connection_pool final
{
    friend struct session_ref;
public:
    explicit connection_pool(boost::asio::io_service& io_service, std::size_t n)
    {
        connections.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
        {
            connections.emplace_back(true, session{io_service});
        }
    }

    explicit connection_pool(std::size_t n) {
        connections.reserve(n);
    }

    void add(session&& s) {
        connections.emplace_back(true, std::move(s));
    }

    session& operator[](std::size_t n) {
        BOOST_ASSERT(n < connections.size());
        return connections[n].second;
    }

    std::size_t size() const noexcept {
        return connections.size();
    }

private:
    std::pair<std::size_t, session*> lease() {
        std::unique_lock<std::mutex> lock{mtx};
        std::size_t free_slot = 0;
        cv.wait(lock, [this, &free_slot]() {
            return find_free(free_slot) == true;
        });
        connections[free_slot].first = false;
        return {free_slot, &(connections[free_slot].second)};
    }

    void give_back(std::size_t n)
    {
        {
            std::lock_guard<std::mutex> lock{mtx};
            BOOST_ASSERT(n < connections.size());
            BOOST_ASSERT(connections[n].first == false);
            connections[n].first = true;
        }
        cv.notify_all();
    }

    bool find_free(std::size_t& pos) const {
        for (std::size_t i = 0; i < connections.size(); i++)
        {
            if (true == connections[i].first) {
                pos = i;
                return true;
            }
        }
        return false;
    }
    std::mutex mtx;
    std::condition_variable cv;
    std::vector<std::pair<bool, session>> connections;
};

struct session_ref final
{
    explicit session_ref(connection_pool& cp) : pool{&cp}
    {
        BOOST_ASSERT(pool);
        auto res = pool->lease();
        lease_id = res.first;
        conn = res.second;
    }
    ~session_ref() {
        BOOST_ASSERT(pool);
        if (lease_id != std::numeric_limits<std::size_t>::max())
            pool->give_back(lease_id);
    }

    explicit operator bool() const noexcept
    {
        BOOST_ASSERT(conn);
        return conn->operator bool();
    }

    auto error_message() const noexcept {
        BOOST_ASSERT(conn);
        return conn->error_message();
    }

    template <typename CompletionHandler>
    void query(const char* query_str, CompletionHandler&& handler) noexcept
    {
        BOOST_ASSERT(conn);
        conn->query(query_str, std::forward<CompletionHandler>(handler));
    }

    session_ref(session_ref&& other) noexcept
        : pool{other.pool}, lease_id{other.lease_id}, conn{other.conn}
    {
        other.lease_id = std::numeric_limits<std::size_t>::max();
    }
    session_ref& operator=(session_ref&& other) noexcept
    {
        pool = other.pool;
        lease_id = other.lease_id;
        other.lease_id = std::numeric_limits<std::size_t>::max();
        conn = other.conn;
        return *this;
    }

    auto last_error_code() const
    {
        return conn->last_error_code();
    }
private:
    connection_pool* pool;
    std::size_t lease_id;
    session* conn;
};

} // namespace postgres
} // namespace db
} // namespace advameg

namespace boost {
namespace system {
template <>
struct is_error_code_enum<advameg::db::postgres::error>
{
    static const bool value = true;
};
} // namespace system
} // namespace boost

#endif /* asyncpq_hpp */
