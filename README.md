# asyncpq
======


##### boost.asio extension for asynchronous PostgreSQL database access.

Website: https://github.com/gwizdz/asyncpq

Modern C++14 header only library for simple and efficient access to PostgreSQL databases.
It features:
* Easy integration with STL.
* Object-relational mapping mechanism for data exchange.
* Multithreading support using connection pool model.
* ...


Requirements:
* [libpq](https://www.postgresql.org/docs/current/static/libpq.html)
* [boost](http://www.boost.org/) - asio, optional, assert, string
  algo, lexical cast, scope exit
* Modern C++14 compiler and standard library (tested with clang, g++)
*...



##### Example 1
```
{
    boost::asio::io_service io_service;
    session db{io_service};
    boost::system::error_code ec;
    
    // for simplicity of an example, opening connection to the db in a synchronous manner
    db.open("dbname=testing", ec);
    if (ec) {
        cout << ec.value() << ":" << ec.message() << endl;
        return;
    }
    cout << db.error_message() << endl;

    BOOST_ASSERT(db);
    db.query("CREATE TABLE test0(id serial PRIMARY KEY, name varchar(50) UNIQUE NOT NULL,"
             "dateCreated timestamp DEFAULT current_timestamp);", [&db](boost::system::error_code const& ec, result r) {
        if (!ec)
        {
            if (!r) {
                cout << "CREATE TABLE failed! - " << db.error_message() << endl;
            }
            else if (r)
                cout << "Table created!" << endl;
        }
        else
        {
            cout << ec.message() << endl;
            cout << r.status() << endl;
        }
    });

    io_service.run();
}

```


##### Example 2
```c++
struct tile_common
{
    std::array<int, 3> coords;
    int series_id;
    boost::optional<std::string> pngdata;
    boost::optional<std::array<float, 4>> mins, maxs, avgs;
};

using tiles_common = std::vector<tile_common>;

```

```c++
#include "datasets.hpp"
#include <experimental/string_view>
#include <advameg/db/asyncpq.hpp>
#include <boost/asio/io_service.hpp>

class data_provider final
{
public:
    explicit data_provider(boost::asio::io_service& io_service,
                           std::size_t size,
                           std::experimental::string_view conn_str);
    ~data_provider();
    
    // ...
    
    void start(std::function<void(boost::system::error_code)>);
    void stop();
    
    // ...
    
    
    void get_tiles
    (
     int serie_id, int zoom_level, std::vector<std::tuple<int, int>> const& missing_tiles,
     std::function<void(boost::system::error_code, tiles_common&&)>
    );
    
private:

	// ...

    boost::asio::io_service& io_service_;
    advameg::db::postgres::connection_pool connection_pool_;
};


// ...

data_provider::data_provider
(
 boost::asio::io_service& io_service,
 std::size_t size,
 std::experimental::string_view conn_str
) : io_service_{io_service}, connection_pool_{io_service_, size}
{
    for (size_t i = 0; i < size; ++i)
    {
        boost::system::error_code ec;
        connection_pool_[i].open(conn_str.to_string().c_str(), ec);
        if (ec)
            BOOST_THROW_EXCEPTION(std::runtime_error{ec.message()});
    }
}

data_provider::~data_provider() = default;

// ...

```

```c++

// ORM mapping function, found via ADL
void map_onto(result::row const& r, tile_common& s)
{
    s.coords = *r.get<std::array<int, 3>>("coords");
    s.series_id = *r.get<int>("series_id");
    if (auto opt_png_data = r.get<std::string>("pngdata"))
    {
        auto& data = *opt_png_data;
        s.pngdata = std::string{std::begin(data) + 2, std::end(data)};
    }
    s.maxs = r.get<std::array<float, 4>>("maxs");
    s.mins = r.get<std::array<float, 4>>("mins");
    s.avgs = r.get<std::array<float, 4>>("avgs");
}

void data_provider::get_tiles
(
 int serie_id, int zoom_level, const std::vector<std::tuple<int, int>>& missing_tiles,
 std::function<void(boost::system::error_code, tiles_common&&)> cont
)
{
    std::ostringstream sql;
    sql << "select * from tiles_common where series_id=" << serie_id << " and coords IN(" << prep_tile_coords_array_query_str(zoom_level, missing_tiles) << ");";
    
    auto session = std::make_shared<db::session_ref>(connection_pool_);
    session->query(sql.str().c_str(), [session, cont](boost::system::error_code ec, db::result r) {
        if (!ec && r) {
            tiles_common tiles;
            const auto num_rows = r.num_rows();
            if (num_rows > 0)
            {
                tiles.reserve(num_rows);
                using std::begin;
                using std::end;
                std::transform(begin(r), end(r), std::back_inserter(tiles), [](advameg::db::postgres::result::row const& row) {
                    tile_common v;
                    row >> v;
                    return v;
                });
            }
            cont(ec, std::move(tiles));
        }
        else {
            if (!r)
                cont(session->last_error_code(), {});
            else
                cont(ec, {});
        }
    });
}
```

To Be Continued...


Copyright (c) 2016 Lukasz Gwizdz
