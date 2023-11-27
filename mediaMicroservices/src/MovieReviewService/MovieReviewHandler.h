#ifndef MEDIA_MICROSERVICES_MOVIEREVIEWHANDLER_H
#define MEDIA_MICROSERVICES_MOVIEREVIEWHANDLER_H

#include <iostream>
#include <string>

#include <mongoc.h>
#include <bson/bson.h>

#include "../../gen-cpp/MovieReviewService.h"
#include "../../gen-cpp/ReviewStorageService.h"
#include "../logger.h"
#include "../tracing.h"
#include "../ClientPool.h"
#include "../RedisClient.h"
#include "../ThriftClient.h"

namespace media_service {
class MovieReviewHandler : public MovieReviewServiceIf {
 public:
  MovieReviewHandler(
      ClientPool<RedisClient> *,
      mongoc_client_pool_t *,
      ClientPool<ThriftClient<ReviewStorageServiceClient>> *);
  ~MovieReviewHandler() override = default;
  void UploadMovieReview(int64_t, const std::string&, int64_t, int64_t,
                         const std::map<std::string, std::string> &) override;
  void ReadMovieReviews(std::vector<Review> & _return, int64_t req_id,
      const std::string& movie_id, int32_t start, int32_t stop, 
      const std::map<std::string, std::string> & carrier) override;
  
 private:
  ClientPool<RedisClient> *_redis_client_pool;
  mongoc_client_pool_t *_mongodb_client_pool;
  ClientPool<ThriftClient<ReviewStorageServiceClient>> *_review_client_pool;
};

MovieReviewHandler::MovieReviewHandler(
    ClientPool<RedisClient> *redis_client_pool,
    mongoc_client_pool_t *mongodb_pool,
    ClientPool<ThriftClient<ReviewStorageServiceClient>> *review_storage_client_pool) {
  _redis_client_pool = redis_client_pool;
  _mongodb_client_pool = mongodb_pool;
  _review_client_pool = review_storage_client_pool;
}

void MovieReviewHandler::UploadMovieReview(
    int64_t req_id,
    const std::string& movie_id,
    int64_t review_id,
    int64_t timestamp,
    const std::map<std::string, std::string> & carrier) {



  // ─────────────────────────────────────────────────────────────────────
  // ─────────────────────────────────────────────────────────────────────
  // ─── Mongodb ─────────────────────────────────────────────────────────
  // ─────────────────────────────────────────────────────────────────────
  // ─────────────────────────────────────────────────────────────────────

  // Initialize local variable for MongoDB
  mongoc_client_t* mongodb_client = nullptr;
  mongoc_cursor_t* cursor         = nullptr;
  bson_error_t error;
  bson_t const* doc_iter          = nullptr;
  bson_t reply                    = BSON_INITIALIZER;
  bool ok                         = true; // Will be reused by different code snippet


  // Init timing stuff
  TextMapReader reader(carrier);
  std::map<std::string, std::string> writer_text_map;
  TextMapWriter writer(writer_text_map);

  // ─── Launch Timing ───────────────────────────────────────────
  auto parent_span = opentracing::Tracer::Global()->Extract(reader);
  auto span = opentracing::Tracer::Global()->StartSpan( "UploadMovieReview", {opentracing::ChildOf(parent_span->get())});
  opentracing::Tracer::Global()->Inject(span->context(), writer);
  // ─────────────────────────────────────────────────────────────

  // ─── Init Connection To Server ───────────────────────────────

  mongodb_client = mongoc_client_pool_pop(_mongodb_client_pool);
  if (!mongodb_client)
  {
    ServiceException se{};
    se.errorCode  = ErrorCode::SE_MONGODB_ERROR;
    se.message    = "Failed to pop a client from MongoDB pool";
    throw se;
  }

  // ─── Accessing Collections ───────────────────────────────────

  // Access the "movie-review" database and the "movie" and "reviews" collections
  mongoc_collection_t* movies_collection   = mongoc_client_get_collection(mongodb_client, "movie-review", "movies");
  mongoc_collection_t* reviews_collection = mongoc_client_get_collection(mongodb_client, "movie-review", "reviews");

  if (!movies_collection || !reviews_collection)
  {
    ServiceException se{};
    se.errorCode = ErrorCode::SE_MONGODB_ERROR;
    se.message = "Failed to get access a collection from a MongoDB pool";


    mongoc_collection_destroy(movies_collection);
    mongoc_collection_destroy(reviews_collection);
    mongoc_client_pool_push(_mongodb_client_pool, mongodb_client);
    throw se;
  }

  // ─── Buidling Documents ──────────────────────────────────────
  // Or the mongoDB queries, sort of
  
  bson_t* movie_doc = BCON_NEW(
    "movie_id", BCON_UTF8(movie_id.c_str())
    );

  bson_t* review_doc = BCON_NEW(
    "review_id",  BCON_INT64(review_id),
    "timestamp",  BCON_INT64(timestamp),
    "movie_id",   BCON_UTF8(movie_id.c_str())
    );


  // ─── Db Find Tracing ─────────────────────────────────────────
  auto find_span = opentracing::Tracer::Global()->StartSpan( "MongoFindMovie", {opentracing::ChildOf(&span->context())}); 

  // Seleting a Movie
  cursor = mongoc_collection_find_with_opts(movies_collection, movie_doc, NULL, NULL);

  find_span->Finish();
  // ─────────────────────────────────────────────────────────────


  // ─── Db Insert Tracing ───────────────────────────────────────
  auto insert_span = opentracing::Tracer::Global()->StartSpan( "MongoMovieTotalInsert", {opentracing::ChildOf(&span->context())});

  // ─── Adding The A Movie If Does Not Exist ─────────────────────

  ok = mongoc_cursor_next(cursor, &doc_iter);
  if(!ok)
  {
    
      if (!mongoc_collection_insert_one(movies_collection, movie_doc, NULL, NULL, &error)) 
      {
        ServiceException se{};
        se.errorCode = ErrorCode::SE_MONGODB_ERROR;
        se.message = error.message;

        //! If you wonder why those lines are here, and also duplicated to
        //! other parts of the source code It's because someone had the mad
        //! idea to mix C and C++. Therefore there is no good way to
        //! disalocate memory using a C-like `goto;` or with a `unique_ptr<>`
        //! in C++ when anything goes out of scope. Also the compiler isn't
        //! keen with any goto  Well done lads.
        bson_destroy(movie_doc);
        bson_destroy(review_doc);
        mongoc_cursor_destroy(cursor);
        mongoc_collection_destroy(movies_collection);
        mongoc_collection_destroy(reviews_collection);
        mongoc_client_pool_push(_mongodb_client_pool, mongodb_client);

        throw se;
      } 
  }

  // ─── Db Update Tracing ───────────────────────────────────────
  auto update_span = opentracing::Tracer::Global()->StartSpan( "MongoMovieReviewInsert", {opentracing::ChildOf(&span->context())});

  // ─── Insert Review Id Anyway ─────────────────────────────────

  if (!mongoc_collection_insert_one(reviews_collection, review_doc, NULL, NULL, &error)) {
    ServiceException se{};
    se.errorCode = ErrorCode::SE_MONGODB_ERROR;
    se.message = error.message;

    bson_destroy(movie_doc);
    bson_destroy(review_doc);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(movies_collection);
    mongoc_collection_destroy(reviews_collection);
    mongoc_client_pool_push(_mongodb_client_pool, mongodb_client);

    throw se;
  }
  
  update_span->Finish();
  insert_span->Finish();
  // ─────────────────────────────────────────────────────────────

  bson_destroy(movie_doc);
  bson_destroy(review_doc);
  mongoc_cursor_destroy(cursor);
  mongoc_collection_destroy(movies_collection);
  mongoc_collection_destroy(reviews_collection);
  mongoc_client_pool_push(_mongodb_client_pool, mongodb_client);


  // ─────────────────────────────────────────────────────────────────────
  // ─────────────────────────────────────────────────────────────────────
  // ─── Redis ───────────────────────────────────────────────────────────
  // ─────────────────────────────────────────────────────────────────────
  // ─────────────────────────────────────────────────────────────────────

  auto redis_client_wrapper = _redis_client_pool->Pop();
  if (!redis_client_wrapper) {
    ServiceException se;
    se.errorCode = ErrorCode::SE_REDIS_ERROR;
    se.message = "Cannot connected to Redis server";
    throw se;
  }
  auto redis_client = redis_client_wrapper->GetClient();
  auto redis_span = opentracing::Tracer::Global()->StartSpan( "RedisUpdate", {opentracing::ChildOf(&span->context())});
  auto num_reviews = redis_client->zcard(movie_id);
  redis_client->sync_commit();
  auto num_reviews_reply = num_reviews.get();
  std::vector<std::string> options{"NX"};
  if (num_reviews_reply.ok() && num_reviews_reply.as_integer())
   
  span->Finish();

}

void MovieReviewHandler::ReadMovieReviews( std::vector<Review> & _return, int64_t req_id, const std::string& movie_id, int32_t start, int32_t stop, const std::map<std::string, std::string> & carrier) {
  
  // Initialize a span
  TextMapReader reader(carrier);
  std::map<std::string, std::string> writer_text_map;
  TextMapWriter writer(writer_text_map);
  auto parent_span = opentracing::Tracer::Global()->Extract(reader);
  auto span = opentracing::Tracer::Global()->StartSpan( "ReadMovieReviews", { opentracing::ChildOf(parent_span->get()) });
  opentracing::Tracer::Global()->Inject(span->context(), writer);


  if (stop <= start || start < 0) { return; }

  // ─────────────────────────────────────────────────────────────────────
  // ─────────────────────────────────────────────────────────────────────
  // ─── Redis ───────────────────────────────────────────────────────────
  // ─────────────────────────────────────────────────────────────────────
  // ─────────────────────────────────────────────────────────────────────

  auto redis_client_wrapper = _redis_client_pool->Pop();
  if (!redis_client_wrapper) {
    ServiceException se;
    se.errorCode = ErrorCode::SE_REDIS_ERROR;
    se.message = "Cannot connected to Redis server";
    throw se;
  }
  auto redis_client = redis_client_wrapper->GetClient();
  auto redis_span = opentracing::Tracer::Global()->StartSpan( "RedisFind", {opentracing::ChildOf(&span->context())});
  auto review_ids_future = redis_client->zrevrange(movie_id, start, stop - 1);
  redis_client->commit();
  redis_span->Finish();

  cpp_redis::reply review_ids_reply;
  try {
    review_ids_reply = review_ids_future.get();
  } catch (...) {
    LOG(error) << "Failed to read review_ids from movie-review-redis";
    _redis_client_pool->Push(redis_client_wrapper);
    throw;
  }
  _redis_client_pool->Push(redis_client_wrapper);
  std::vector<int64_t> review_ids;
  auto review_ids_reply_array = review_ids_reply.as_array();
  for (auto &review_id_reply : review_ids_reply_array) {
    review_ids.emplace_back(std::stoul(review_id_reply.as_string()));
  }

  // ─────────────────────────────────────────────────────────────
  // ─────────────────────────────────────────────────────────────
  // ─── MongoDB Stuff ───────────────────────────────────────────
  // ─────────────────────────────────────────────────────────────
  // ─────────────────────────────────────────────────────────────

  // Initialize local variable for MongoDB
  mongoc_collection_t*  reviews_collection  = nullptr;
  mongoc_client_t* mongodb_client           = nullptr;
  mongoc_cursor_t* cursor                   = nullptr;
  bson_t const* doc_el                      = nullptr;
  bson_error_t error;
  bson_iter_t iter;
  bson_t reply                    = BSON_INITIALIZER;

  std::multimap<std::string, std::string> redis_update_map;

  unsigned int mongo_start = start + review_ids.size();

  // If the index are not broken
  if (mongo_start < stop)
  {

    // ─── Init Connection To Server ───────────────────────────────

    mongodb_client = mongoc_client_pool_pop(_mongodb_client_pool);
    if (!mongodb_client)
    {
      ServiceException se{};
      se.errorCode  = ErrorCode::SE_MONGODB_ERROR;
      se.message    = "Failed to pop a client from MongoDB pool";
      throw se;
    }

    // ─── Accessing Collections ───────────────────────────────────

    // Access the "movie-review" database and "reviews" collections
    reviews_collection = mongoc_client_get_collection(mongodb_client, "movie-review", "reviews");

    if (!reviews_collection)
    {
      ServiceException se{};
      se.errorCode = ErrorCode::SE_MONGODB_ERROR;
      se.message = "Failed to get access a collection from a MongoDB pool";

      mongoc_collection_destroy(reviews_collection);
      mongoc_client_pool_push(_mongodb_client_pool, mongodb_client);

      throw se;
    }

  }

  // ─── Buidling Documents ──────────────────────────────────────
  // Or the mongoDB queries, sort of
    
  bson_t* movie_doc = BCON_NEW(
    "movie_id", BCON_UTF8(movie_id.c_str())
    );

  bson_t* opts = BCON_NEW(
    "limit", BCON_INT32(stop),
    "skip", BCON_INT32(0),
    "sort", "{ timestamp: -1 }" 
    );


  // ─────────────────────────────────────────────────────────────
  // ─── Gather Result From Db To Update Redis ───────────────────
  // ─────────────────────────────────────────────────────────────

  // Cursor  and cursor index counter
  size_t cursor_idx = 0; 

  // ─── Start Trace ─────────────────────────────────────────────    
  auto find_span = opentracing::Tracer::Global()->StartSpan( "MongoFindMovieReviews", {opentracing::ChildOf(&span->context())});

  // Run the find query
  cursor = mongoc_collection_find_with_opts(reviews_collection, movie_doc, opts, NULL);

  find_span->Finish();
  // ─────────────────────────────────────────────────────────────

  // ─── Iterrate Over All Gathered Item Of The Collection ───────
  while (mongoc_cursor_next(cursor, &doc_el))
  {
    // Create placeholder for review element
    int32_t review_id;
    int64_t timestamp;

    // Find in the BSON the entries we want
    if (bson_iter_init_find(&iter, doc_el, "review_id") && BSON_ITER_HOLDS_INT32(&iter)) 
      review_id = bson_iter_int32(&iter);
    //! THIS SHOULD NEVER HAPPEND
    else {

      ServiceException se{};
      se.errorCode = ErrorCode::SE_MONGODB_ERROR;
      se.message = "Cannot read collection entry 'review_id'";

      bson_destroy(opts);
      bson_destroy(movie_doc);
      mongoc_cursor_destroy(cursor);
      mongoc_collection_destroy(reviews_collection);
      mongoc_client_pool_push(_mongodb_client_pool, mongodb_client);
      
      throw se;
    }

    if (bson_iter_find(&iter, "timestamp") && BSON_ITER_HOLDS_INT64(&iter)) 
      timestamp = bson_iter_int64(&iter);
    //! THIS SHOULD NEVER HAPPEND
    else {

      ServiceException se{};
      se.errorCode = ErrorCode::SE_MONGODB_ERROR;
      se.message = "Cannot read collection entry 'timestamp'";

      bson_destroy(opts);
      bson_destroy(movie_doc);
      mongoc_cursor_destroy(cursor);
      mongoc_collection_destroy(reviews_collection);
      mongoc_client_pool_push(_mongodb_client_pool, mongodb_client);
      
      throw se;
    }

    //? If the position of the entries (index) are in certain range we add them
    //? to vectors/map that will be used to update Redis
    if (cursor_idx >= mongo_start) review_ids.emplace_back(review_id);
    redis_update_map.insert({std::to_string(timestamp), std::to_string(review_id)});

    // Increment cursor after iteration
    ++cursor_idx;
  }


  find_span->Finish();

  bson_destroy(opts);
  bson_destroy(movie_doc);
  mongoc_cursor_destroy(cursor);
  mongoc_collection_destroy(reviews_collection);
  mongoc_client_pool_push(_mongodb_client_pool, mongodb_client);


  // ─────────────────────────────────────────────────────────────
  // ─── Back To Redis Shit ──────────────────────────────────────
  // ─────────────────────────────────────────────────────────────

  std::future<std::vector<Review>> review_future = std::async(
    std::launch::async, [&]() {
      auto review_client_wrapper = _review_client_pool->Pop();
      if (!review_client_wrapper) {
        ServiceException se;
        se.errorCode = ErrorCode::SE_THRIFT_CONN_ERROR;
        se.message = "Failed to connected to review-storage-service";
        throw se;
      }
      std::vector<Review> _return_reviews;
      auto review_client = review_client_wrapper->GetClient();
      try {
        review_client->ReadReviews(
            _return_reviews, req_id, review_ids, writer_text_map);
      } catch (...) {
        _review_client_pool->Push(review_client_wrapper);
        LOG(error) << "Failed to read review from review-storage-service";
        throw;
      }
      _review_client_pool->Push(review_client_wrapper);
      return _return_reviews;
    });

  std::future<cpp_redis::reply> zadd_reply_future;
  if (!redis_update_map.empty()) {
    // Update Redis
    redis_client_wrapper = _redis_client_pool->Pop();
    if (!redis_client_wrapper) {
      ServiceException se;
      se.errorCode = ErrorCode::SE_REDIS_ERROR;
      se.message = "Cannot connected to Redis server";
      throw se;
    }
    redis_client = redis_client_wrapper->GetClient();
    auto redis_update_span = opentracing::Tracer::Global()->StartSpan(
        "RedisUpdate", {opentracing::ChildOf(&span->context())});
    redis_client->del(std::vector<std::string>{movie_id});
    std::vector<std::string> options{"NX"};
    zadd_reply_future = redis_client->zadd(
        movie_id, options, redis_update_map);
    redis_client->commit();
    redis_update_span->Finish();
  }

  try {
    _return = review_future.get();
  } catch (...) {
    LOG(error) << "Failed to get review from review-storage-service";
    if (!redis_update_map.empty()) {
      try {
        zadd_reply_future.get();
      } catch (...) {
        LOG(error) << "Failed to Update Redis Server";
      }
      _redis_client_pool->Push(redis_client_wrapper);
    }
    throw;
  }

  if (!redis_update_map.empty()) {
    try {
      zadd_reply_future.get();
    } catch (...) {
      LOG(error) << "Failed to Update Redis Server";
      _redis_client_pool->Push(redis_client_wrapper);
      throw;
    }
    _redis_client_pool->Push(redis_client_wrapper);
  }

  span->Finish();
  
}

} // namespace media_service


#endif //MEDIA_MICROSERVICES_MOVIEREVIEWHANDLER_H
