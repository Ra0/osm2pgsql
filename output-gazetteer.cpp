#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libpq-fe.h>
#include <boost/make_shared.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include "osmtypes.hpp"
#include "middle.hpp"
#include "pgsql.hpp"
#include "reprojection.hpp"
#include "output-gazetteer.hpp"
#include "options.hpp"
#include "util.hpp"

#define SRID (reproj->project_getprojinfo()->srs)

#define CREATE_KEYVALUETYPE_TYPE                \
   "CREATE TYPE keyvalue AS ("                  \
   "  key TEXT,"                                \
   "  value TEXT"                               \
   ")"

#define CREATE_WORDSCORE_TYPE                   \
   "CREATE TYPE wordscore AS ("                 \
   "  word TEXT,"                               \
   "  score FLOAT"                              \
   ")"

#define CREATE_PLACE_TABLE                      \
   "CREATE TABLE place ("                       \
   "  osm_type CHAR(1) NOT NULL,"               \
   "  osm_id " POSTGRES_OSMID_TYPE " NOT NULL," \
   "  class TEXT NOT NULL,"                     \
   "  type TEXT NOT NULL,"                      \
   "  name HSTORE,"                             \
   "  admin_level INTEGER,"                     \
   "  housenumber TEXT,"                        \
   "  street TEXT,"                             \
   "  addr_place TEXT,"                         \
   "  isin TEXT,"                               \
   "  postcode TEXT,"                           \
   "  country_code VARCHAR(2),"                 \
   "  extratags HSTORE"                         \
   ") %s %s"

#define ADMINLEVEL_NONE 100

#define CREATE_PLACE_ID_INDEX \
   "CREATE INDEX place_id_idx ON place USING BTREE (osm_type, osm_id) %s %s"

#define TAGINFO_NODE 0x1u
#define TAGINFO_WAY  0x2u
#define TAGINFO_AREA 0x4u

void output_gazetteer_t::require_slim_mode(void)
{
   if (!m_options.slim)
   {
      fprintf(stderr, "Cannot apply diffs unless in slim mode\n");
      util::exit_nicely();
   }

   return;
}

void output_gazetteer_t::copy_data(const char *sql)
{
   unsigned int sqlLen = strlen(sql);

   /* Make sure we have an active copy */
   if (!CopyActive)
   {
      pgsql_exec(Connection, PGRES_COPY_IN, "COPY place (osm_type, osm_id, class, type, name, admin_level, housenumber, street, addr_place, isin, postcode, country_code, extratags, geometry) FROM STDIN");
      CopyActive = 1;
   }

   /* If the combination of old and new data is too big, flush old data */
   if (BufferLen + sqlLen > BUFFER_SIZE - 10)
   {
      pgsql_CopyData("place", Connection, Buffer);
      BufferLen = 0;
   }

   /*
    * If new data by itself is too big, output it immediately,
    * otherwise just add it to the buffer.
    */
   if (sqlLen > BUFFER_SIZE - 10)
   {
      pgsql_CopyData("Place", Connection, sql);
      sqlLen = 0;
   }
   else if (sqlLen > 0)
   {
      strcpy(Buffer + BufferLen, sql);
      BufferLen += sqlLen;
      sqlLen = 0;
   }

   /* If we have completed a line, output it */
   if (BufferLen > 0 && Buffer[BufferLen-1] == '\n')
   {
      pgsql_CopyData("place", Connection, Buffer);
      BufferLen = 0;
   }

   return;
}

void output_gazetteer_t::stop_copy(void)
{
   PGresult *res;

   /* Do we have a copy active? */
   if (!CopyActive) return;

   /* Terminate the copy */
   if (PQputCopyEnd(Connection, NULL) != 1)
   {
      fprintf(stderr, "COPY_END for place failed: %s\n", PQerrorMessage(Connection));
      util::exit_nicely();
   }

   /* Check the result */
   res = PQgetResult(Connection);
   if (PQresultStatus(res) != PGRES_COMMAND_OK)
   {
      fprintf(stderr, "COPY_END for place failed: %s\n", PQerrorMessage(Connection));
      PQclear(res);
      util::exit_nicely();
   }

   /* Discard the result */
   PQclear(res);

   /* We no longer have an active copy */
   CopyActive = 0;

   return;
}

#if 0
static void copy_error_data(const char *sql)
{
   unsigned int sqlLen = strlen(sql);

   /* Make sure we have an active copy */
   if (!CopyErrorActive)
   {
      pgsql_exec(ConnectionError, PGRES_COPY_IN, "COPY import_polygon_error (osm_type, osm_id, class, type, name, country_code, updated, errormessage, prevgeometry, newgeometry) FROM stdin;");
      CopyErrorActive = 1;
   }

   /* If the combination of old and new data is too big, flush old data */
   if (BufferErrorLen + sqlLen > BUFFER_SIZE - 10)
   {
      pgsql_CopyData("import_polygon_error", ConnectionError, BufferError);
      BufferErrorLen = 0;
   }

   /*
    * If new data by itself is too big, output it immediately,
    * otherwise just add it to the buffer.
    */
   if (sqlLen > BUFFER_SIZE - 10)
   {
      pgsql_CopyData("import_polygon_error", ConnectionError, sql);
      sqlLen = 0;
   }
   else if (sqlLen > 0)
   {
      strcpy(BufferError + BufferErrorLen, sql);
      BufferErrorLen += sqlLen;
      sqlLen = 0;
   }

   /* If we have completed a line, output it */
   if (BufferErrorLen > 0 && BufferError[BufferErrorLen-1] == '\n')
   {
      pgsql_CopyData("place", ConnectionError, BufferError);
      BufferErrorLen = 0;
   }

   return;
}

static void stop_error_copy(void)
{
   PGresult *res;

   /* Do we have a copy active? */
   if (!CopyErrorActive) return;

   /* Terminate the copy */
   if (PQputCopyEnd(ConnectionError, NULL) != 1)
   {
      fprintf(stderr, "COPY_END for import_polygon_error failed: %s\n", PQerrorMessage(ConnectionError));
      util::exit_nicely();
   }

   /* Check the result */
   res = PQgetResult(ConnectionError);
   if (PQresultStatus(res) != PGRES_COMMAND_OK)
   {
      fprintf(stderr, "COPY_END for import_polygon_error failed: %s\n", PQerrorMessage(ConnectionError));
      PQclear(res);
      util::exit_nicely();
   }

   /* Discard the result */
   PQclear(res);

   /* We no longer have an active copy */
   CopyErrorActive = 0;

   return;
}
#endif

static int split_tags(struct keyval *tags, unsigned int flags,
                      struct keyval *names, struct keyval *places,
                      struct keyval *extratags, int* admin_level,
                      struct keyval ** housenumber, struct keyval ** street,
                      struct keyval ** addr_place, char ** isin,
                      struct keyval ** postcode, struct keyval ** countrycode)
{
   size_t subval;
   int placehouse = 0;
   int placebuilding = 0;
   int placeadmin = 0;
   struct keyval *landuse;
   struct keyval *place;
   struct keyval *item;
   struct keyval *conscriptionnumber;
   struct keyval *streetnumber;

   *admin_level = ADMINLEVEL_NONE;
   *housenumber = 0;
   *street = 0;
   *addr_place = 0;
   *isin = 0;
   int isinsize = 0;
   *postcode = 0;
   *countrycode = 0;
   landuse = 0;
   place = 0;
   conscriptionnumber = 0;
   streetnumber = 0;

   /* Initialise the result lists */
   keyval::initList(names);
   keyval::initList(places);
   keyval::initList(extratags);

   /* Loop over the tags */
   while ((item = keyval::popItem(tags)) != NULL)
   {

      /* If this is a name tag, add it to the name list */
      if (strcmp(item->key, "ref") == 0 ||
          strcmp(item->key, "int_ref") == 0 ||
          strcmp(item->key, "nat_ref") == 0 ||
          strcmp(item->key, "reg_ref") == 0 ||
          strcmp(item->key, "loc_ref") == 0 ||
          strcmp(item->key, "old_ref") == 0 ||
          strcmp(item->key, "ncn_ref") == 0 ||
          strcmp(item->key, "rcn_ref") == 0 ||
          strcmp(item->key, "lcn_ref") == 0 ||
          strcmp(item->key, "iata") == 0 ||
          strcmp(item->key, "icao") == 0 ||
          strcmp(item->key, "pcode:1") == 0 ||
          strcmp(item->key, "pcode:2") == 0 ||
          strcmp(item->key, "pcode:3") == 0 ||
          strcmp(item->key, "un:pcode:1") == 0 ||
          strcmp(item->key, "un:pcode:2") == 0 ||
          strcmp(item->key, "un:pcode:3") == 0 ||
          strcmp(item->key, "name") == 0 ||
          (strncmp(item->key, "name:", 5) == 0) ||
          strcmp(item->key, "int_name") == 0 ||
          (strncmp(item->key, "int_name:", 9) == 0) ||
          strcmp(item->key, "nat_name") == 0 ||
          (strncmp(item->key, "nat_name:", 9) == 0) ||
          strcmp(item->key, "reg_name") == 0 ||
          (strncmp(item->key, "reg_name:", 9) == 0) ||
          strcmp(item->key, "loc_name") == 0 ||
          (strncmp(item->key, "loc_name:", 9) == 0) ||
          strcmp(item->key, "old_name") == 0 ||
          (strncmp(item->key, "old_name:", 9) == 0) ||
          strcmp(item->key, "alt_name") == 0 ||
          (strncmp(item->key, "alt_name_", 9) == 0) ||
          (strncmp(item->key, "alt_name:", 9) == 0) ||
          strcmp(item->key, "official_name") == 0 ||
          (strncmp(item->key, "official_name:", 14) == 0) ||
          strcmp(item->key, "commonname") == 0 ||
          (strncmp(item->key, "commonname:", 11) == 0) ||
          strcmp(item->key, "common_name") == 0 ||
          (strncmp(item->key, "common_name:", 12) == 0) ||
          strcmp(item->key, "place_name") == 0 ||
          (strncmp(item->key, "place_name:", 11) == 0) ||
          strcmp(item->key, "short_name") == 0 ||
          (strncmp(item->key, "short_name:", 11) == 0) ||
          strcmp(item->key, "operator") == 0) /* operator is a bit of an oddity */
      {
         if (strcmp(item->key, "name:prefix") == 0)
         {
            keyval::pushItem(extratags, item);
         }
         else
         {
            keyval::pushItem(names, item);
         }
      }
      else if (strcmp(item->key, "emergency") == 0 ||
               strcmp(item->key, "tourism") == 0 ||
               strcmp(item->key, "historic") == 0 ||
               strcmp(item->key, "military") == 0 ||
               strcmp(item->key, "natural") == 0)
      {
         if (strcmp(item->value, "no") && strcmp(item->value, "yes"))
         {
            keyval::pushItem(places, item);
         }
         else
         {
            keyval::freeItem(item);
         }
      }
      else if (strcmp(item->key, "highway") == 0)
      {
         if (strcmp(item->value, "no") &&
             strcmp(item->value, "turning_circle") &&
             strcmp(item->value, "traffic_signals") &&
             strcmp(item->value, "mini_roundabout") &&
             strcmp(item->value, "noexit") &&
             strcmp(item->value, "crossing"))
         {
             keyval::pushItem(places, item);
         }
         else
         {
             keyval::freeItem(item);
         }
      }
      else if (strcmp(item->key, "aerialway") == 0 ||
               strcmp(item->key, "aeroway") == 0 ||
               strcmp(item->key, "amenity") == 0 ||
               strcmp(item->key, "boundary") == 0 ||
               strcmp(item->key, "bridge") == 0 ||
               strcmp(item->key, "craft") == 0 ||
               strcmp(item->key, "leisure") == 0 ||
               strcmp(item->key, "office") == 0 ||
               strcmp(item->key, "railway") == 0 ||
               strcmp(item->key, "shop") == 0 ||
               strcmp(item->key, "tunnel") == 0 )
      {
         if (strcmp(item->value, "no"))
         {
            keyval::pushItem(places, item);
            if (strcmp(item->key, "boundary") == 0 && strcmp(item->value, "administrative") == 0)
            {
               placeadmin = 1;
            }
         }
         else
         {
            keyval::freeItem(item);
         }
      }
      else if (strcmp(item->key, "waterway") == 0 &&
               strcmp(item->value, "riverbank") != 0)
      {
            keyval::pushItem(places, item);
      }
      else if (strcmp(item->key, "place") == 0)
      {
         place = item;
      }
      else if (strcmp(item->key, "addr:housename") == 0)
      {
         keyval::pushItem(names, item);
         placehouse = 1;
      }
      else if (strcmp(item->key, "landuse") == 0)
      {
         if (strcmp(item->value, "cemetery") == 0)
            keyval::pushItem(places, item);
         else
            landuse = item;
      }
      else if (strcmp(item->key, "postal_code") == 0 ||
          strcmp(item->key, "post_code") == 0 ||
          strcmp(item->key, "postcode") == 0 ||
          strcmp(item->key, "addr:postcode") == 0 ||
          strcmp(item->key, "tiger:zip_left") == 0 ||
          strcmp(item->key, "tiger:zip_right") == 0)
      {
         if (*postcode)
	        keyval::freeItem(item);
         else
            *postcode = item;
      }
      else if (strcmp(item->key, "addr:street") == 0)
      {
         *street = item;
      }
      else if (strcmp(item->key, "addr:place") == 0)
      {
         *addr_place = item;
      }
      else if ((strcmp(item->key, "country_code_iso3166_1_alpha_2") == 0 ||
                strcmp(item->key, "country_code_iso3166_1") == 0 ||
                strcmp(item->key, "country_code_iso3166") == 0 ||
                strcmp(item->key, "country_code") == 0 ||
                strcmp(item->key, "iso3166-1:alpha2") == 0 ||
                strcmp(item->key, "iso3166-1") == 0 ||
                strcmp(item->key, "ISO3166-1") == 0 ||
                strcmp(item->key, "iso3166") == 0 ||
                strcmp(item->key, "is_in:country_code") == 0 ||
                strcmp(item->key, "addr:country") == 0 ||
                strcmp(item->key, "addr:country_code") == 0)
                && strlen(item->value) == 2)
      {
         *countrycode = item;
      }
      else if (strcmp(item->key, "addr:housenumber") == 0)
      {
          /* house number can be far more complex than just a single house number - leave for postgresql to deal with */
         if (*housenumber)
             keyval::freeItem(item);
         else {
             *housenumber = item;
             placehouse = 1;
         }
      }
      else if (strcmp(item->key, "addr:conscriptionnumber") == 0)
      {
         if (conscriptionnumber)
             keyval::freeItem(item);
         else {
             conscriptionnumber = item;
             placehouse = 1;
         }
      }
      else if (strcmp(item->key, "addr:streetnumber") == 0)
      {
         if (streetnumber)
             keyval::freeItem(item);
         else {
             streetnumber = item;
             placehouse = 1;
         }
      }
      else if (strcmp(item->key, "addr:interpolation") == 0)
      {
          /* house number can be far more complex than just a single house number - leave for postgresql to deal with */
          if (*housenumber) {
              keyval::freeItem(item);
          } else {
             *housenumber = item;
             keyval::addItem(places, "place", "houses", 1);
          }
      }
      else if (strcmp(item->key, "tiger:county") == 0)
      {
         /* strip the state and replace it with a county suffix to ensure that
            the tag only matches against counties and not against some town
            with the same name.
          */
         subval = strcspn(item->value, ",");
         *isin = (char *)realloc(*isin, isinsize + 9 + subval);
         *(*isin+isinsize) = ',';
         strncpy(*isin+1+isinsize, item->value, subval);
         strcpy(*isin+1+isinsize+subval, " county");
         isinsize += 8 + subval;
         keyval::freeItem(item);
      }
      else if (strcmp(item->key, "is_in") == 0 ||
          (strncmp(item->key, "is_in:", 5) == 0) ||
          strcmp(item->key, "addr:suburb")== 0 ||
          strcmp(item->key, "addr:county")== 0 ||
          strcmp(item->key, "addr:city") == 0 ||
          strcmp(item->key, "addr:state_code") == 0 ||
          strcmp(item->key, "addr:state") == 0)
      {
          *isin = (char *)realloc(*isin, isinsize + 2 + strlen(item->value));
         *(*isin+isinsize) = ',';
         strcpy(*isin+1+isinsize, item->value);
         isinsize += 1 + strlen(item->value);
         keyval::freeItem(item);
      }
      else if (strcmp(item->key, "admin_level") == 0)
      {
         *admin_level = atoi(item->value);
         keyval::freeItem(item);
      }
      else if (strcmp(item->key, "tracktype") == 0 ||
               strcmp(item->key, "traffic_calming") == 0 ||
               strcmp(item->key, "service") == 0 ||
               strcmp(item->key, "cuisine") == 0 ||
               strcmp(item->key, "capital") == 0 ||
               strcmp(item->key, "dispensing") == 0 ||
               strcmp(item->key, "religion") == 0 ||
               strcmp(item->key, "denomination") == 0 ||
               strcmp(item->key, "sport") == 0 ||
               strcmp(item->key, "internet_access") == 0 ||
               strcmp(item->key, "lanes") == 0 ||
               strcmp(item->key, "surface") == 0 ||
               strcmp(item->key, "smoothness") == 0 ||
               strcmp(item->key, "width") == 0 ||
               strcmp(item->key, "est_width") == 0 ||
               strcmp(item->key, "incline") == 0 ||
               strcmp(item->key, "opening_hours") == 0 ||
               strcmp(item->key, "food_hours") == 0 ||
               strcmp(item->key, "collection_times") == 0 ||
               strcmp(item->key, "service_times") == 0 ||
               strcmp(item->key, "smoking_hours") == 0 ||
               strcmp(item->key, "disused") == 0 ||
               strcmp(item->key, "wheelchair") == 0 ||
               strcmp(item->key, "sac_scale") == 0 ||
               strcmp(item->key, "trail_visibility") == 0 ||
               strcmp(item->key, "mtb:scale") == 0 ||
               strcmp(item->key, "mtb:description") == 0 ||
               strcmp(item->key, "wood") == 0 ||
               strcmp(item->key, "drive_thru") == 0 ||
               strcmp(item->key, "drive_in") == 0 ||
               strcmp(item->key, "access") == 0 ||
               strcmp(item->key, "vehicle") == 0 ||
               strcmp(item->key, "bicyle") == 0 ||
               strcmp(item->key, "foot") == 0 ||
               strcmp(item->key, "goods") == 0 ||
               strcmp(item->key, "hgv") == 0 ||
               strcmp(item->key, "motor_vehicle") == 0 ||
               strcmp(item->key, "motor_car") == 0 ||
               (strncmp(item->key, "access:", 7) == 0) ||
               (strncmp(item->key, "contact:", 8) == 0) ||
               (strncmp(item->key, "drink:", 6) == 0) ||
               strcmp(item->key, "oneway") == 0 ||
               strcmp(item->key, "date_on") == 0 ||
               strcmp(item->key, "date_off") == 0 ||
               strcmp(item->key, "day_on") == 0 ||
               strcmp(item->key, "day_off") == 0 ||
               strcmp(item->key, "hour_on") == 0 ||
               strcmp(item->key, "hour_off") == 0 ||
               strcmp(item->key, "maxweight") == 0 ||
               strcmp(item->key, "maxheight") == 0 ||
               strcmp(item->key, "maxspeed") == 0 ||
               strcmp(item->key, "disused") == 0 ||
               strcmp(item->key, "toll") == 0 ||
               strcmp(item->key, "charge") == 0 ||
               strcmp(item->key, "population") == 0 ||
               strcmp(item->key, "description") == 0 ||
               strcmp(item->key, "image") == 0 ||
               strcmp(item->key, "attribution") == 0 ||
               strcmp(item->key, "fax") == 0 ||
               strcmp(item->key, "email") == 0 ||
               strcmp(item->key, "url") == 0 ||
               strcmp(item->key, "website") == 0 ||
               strcmp(item->key, "phone") == 0 ||
               strcmp(item->key, "tel") == 0 ||
               strcmp(item->key, "real_ale") == 0 ||
               strcmp(item->key, "smoking") == 0 ||
               strcmp(item->key, "food") == 0 ||
               strcmp(item->key, "camera") == 0 ||
               strcmp(item->key, "brewery") == 0 ||
               strcmp(item->key, "locality") == 0 ||
               strcmp(item->key, "wikipedia") == 0 ||
               (strncmp(item->key, "wikipedia:", 10) == 0)
               )
      {
          keyval::pushItem(extratags, item);
      }
      else if (strcmp(item->key, "building") == 0)
      {
          placebuilding = 1;
          keyval::freeItem(item);
      }
      else if (strcmp(item->key, "mountain_pass") == 0)
      {
          keyval::pushItem(places, item);
      }
      else
      {
         keyval::freeItem(item);
      }
   }

   /* Handle Czech/Slovak addresses:
        - if we have just a conscription number or a street number,
          just use the one we have as a house number
        - if we have both of them, concatenate them so users may search
          by any of them
    */
   if (conscriptionnumber || streetnumber)
   {
      if (*housenumber)
      {
         keyval::freeItem(*housenumber);
      }
      if (!conscriptionnumber)
      {
         keyval::addItem(tags, "addr:housenumber", streetnumber->value, 0);
         keyval::freeItem(streetnumber);
         *housenumber = keyval::popItem(tags);
      }
      if (!streetnumber)
      {
         keyval::addItem(tags, "addr:housenumber", conscriptionnumber->value, 10);
         keyval::freeItem(conscriptionnumber);
         *housenumber = keyval::popItem(tags);
      }
      if (conscriptionnumber && streetnumber)
      {
         char * completenumber = strdup(conscriptionnumber->value);
         size_t completenumberlength = strlen(completenumber);
         completenumber = (char *)realloc(completenumber, completenumberlength + 2 + strlen(streetnumber->value));
         *(completenumber + completenumberlength) = '/';
         strcpy(completenumber + completenumberlength + 1, streetnumber->value);
         keyval::freeItem(conscriptionnumber);
         keyval::freeItem(streetnumber);
         keyval::addItem(tags, "addr:housenumber", completenumber, 0);
         *housenumber = keyval::popItem(tags);
         free(completenumber);
      }
    }

   if (place)
   {
      if (placeadmin)
      {
         keyval::pushItem(extratags, place);
      }
      else
      {
         keyval::pushItem(places, place);
      }
   }

   if (placehouse && !keyval::listHasData(places))
   {
      keyval::addItem(places, "place", "house", 1);
   }

   /* Fallback place types - only used if we didn't create something more specific already */
   if (placebuilding && !keyval::listHasData(places) && (keyval::listHasData(names) || *housenumber || *postcode))
   {
      keyval::addItem(places, "building", "yes", 1);
   }

   if (landuse)
   {
      if (!keyval::listHasData(places) && keyval::listHasData(names))
      {
          keyval::pushItem(places, landuse);
      }
      else
      {
          keyval::freeItem(landuse);
      }
   }

   if (*postcode && !keyval::listHasData(places))
   {
      keyval::addItem(places, "place", "postcode", 1);
   }

   /* Try to convert everything to an area */
   return 1;
}

void escape_array_record(char *out, int len, const char *in)
{
    int count = 0;
    const char *old_in = in, *old_out = out;

    if (!len)
        return;

    while(*in && count < len-3) {
        switch(*in) {
            case '\\': *out++ = '\\'; *out++ = '\\'; *out++ = '\\'; *out++ = '\\'; *out++ = '\\'; *out++ = '\\'; *out++ = '\\'; *out++ = '\\'; count+= 8; break;
            case '\n':
            case '\r':
            case '\t':
            case '"':
                /* This is a bit naughty - we know that nominatim ignored these characters so just drop them now for simplicity */
		*out++ = ' '; count++; break;
            default:   *out++ = *in; count++; break;
        }
        in++;
    }
    *out = '\0';

    if (*in)
        fprintf(stderr, "%s truncated at %d chars: %s\n%s\n", __FUNCTION__, count, old_in, old_out);
}

void output_gazetteer_t::delete_unused_classes(char osm_type, osmid_t osm_id, struct keyval *places) {
    int i,sz, slen;
    PGresult   *res;
    char tmp[16];
    char tmp2[2];
    char *cls, *clslist = 0;
    char const *paramValues[2];

    tmp2[0] = osm_type; tmp2[1] = '\0';
    paramValues[0] = tmp2;
    snprintf(tmp, sizeof(tmp), "%" PRIdOSMID, osm_id);
    paramValues[1] = tmp;
    res = pgsql_execPrepared(ConnectionDelete, "get_classes", 2, paramValues, PGRES_TUPLES_OK);

    sz = PQntuples(res);
    if (sz > 0 && !places) {
        PQclear(res);
        /* uncondtional delete of all places */
        stop_copy();
        pgsql_exec(Connection, PGRES_COMMAND_OK, "DELETE FROM place WHERE osm_type = '%c' AND osm_id  = %" PRIdOSMID, osm_type, osm_id);
    } else {
        for (i = 0; i < sz; i++) {
            cls = PQgetvalue(res, i, 0);
            if (!keyval::getItem(places, cls)) {
                if (!clslist) {
                    clslist = (char *)malloc(strlen(cls)+3);
                    sprintf(clslist, "'%s'", cls);
                } else {
                    slen = strlen(clslist);
                    clslist = (char *)realloc(clslist, slen + 4 + strlen(cls));
                    sprintf(&(clslist[slen]), ",'%s'", cls);
                }
            }
        }

        PQclear(res);

        if (clslist) {
           /* Stop any active copy */
           stop_copy();

           /* Delete all places for this object */
           pgsql_exec(Connection, PGRES_COMMAND_OK, "DELETE FROM place WHERE osm_type = '%c' AND osm_id = %"
        PRIdOSMID " and class = any(ARRAY[%s])", osm_type, osm_id, clslist);
           free(clslist);
        }
    }
}

void output_gazetteer_t::add_place(char osm_type, osmid_t osm_id, const char *key_class, const char *type, struct keyval *names, struct keyval *extratags,
   int adminlevel, struct keyval *housenumber, struct keyval *street, struct keyval *addr_place, const char *isin, struct keyval *postcode, struct keyval *countrycode, const char *wkt)
{
   int first;
   struct keyval *name;
   char sql[2048];

   /* Output a copy line for this place */
   sprintf(sql, "%c\t%" PRIdOSMID "\t", osm_type, osm_id);
   copy_data(sql);

   escape(sql, sizeof(sql), key_class);
   copy_data(sql);
   copy_data("\t");

   escape(sql, sizeof(sql), type);
   copy_data(sql);
   copy_data("\t");

   /* start name array */
   if (keyval::listHasData(names))
   {
      first = 1;
      for (name = keyval::firstItem(names); name; name = keyval::nextItem(names, name))
      {
         if (first) first = 0;
         else copy_data(", ");

         copy_data("\"");

         escape_array_record(sql, sizeof(sql), name->key);
         copy_data(sql);

         copy_data("\"=>\"");

         escape_array_record(sql, sizeof(sql), name->value);
         copy_data(sql);

         copy_data("\"");
      }
      copy_data("\t");
   }
   else
   {
      copy_data("\\N\t");
   }

   sprintf(sql, "%d\t", adminlevel);
   copy_data(sql);

   if (housenumber)
   {
      escape(sql, sizeof(sql), housenumber->value);
      copy_data(sql);
      copy_data("\t");
   }
   else
   {
      copy_data("\\N\t");
   }

   if (street)
   {
      escape(sql, sizeof(sql), street->value);
      copy_data(sql);
      copy_data("\t");
   }
   else
   {
      copy_data("\\N\t");
   }

   if (addr_place)
   {
      escape(sql, sizeof(sql), addr_place->value);
      copy_data(sql);
      copy_data("\t");
   }
   else
   {
      copy_data("\\N\t");
   }

   if (isin)
   {
       /* Skip the leading ',' from the contactination */
      escape(sql, sizeof(sql), isin+1);
      copy_data(sql);
      copy_data("\t");
   }
   else
   {
      copy_data("\\N\t");
   }

   if (postcode)
   {
      escape(sql, sizeof(sql), postcode->value);
      copy_data(sql);
      copy_data("\t");
   }
   else
   {
      copy_data("\\N\t");
   }

   if (countrycode)
   {
      escape(sql, sizeof(sql), countrycode->value);
      copy_data(sql);
      copy_data("\t");
   }
   else
   {
     copy_data("\\N\t");
   }

   /* extra tags array */
   if (keyval::listHasData(extratags))
   {
      first = 1;
      for (name = keyval::firstItem(extratags); name; name = keyval::nextItem(extratags, name))
      {
         if (first) first = 0;
         else copy_data(", ");

         copy_data("\"");

         escape_array_record(sql, sizeof(sql), name->key);
         copy_data(sql);

         copy_data("\"=>\"");

         escape_array_record(sql, sizeof(sql), name->value);
         copy_data(sql);

         copy_data("\"");
      }
      copy_data("\t");
   }
   else
   {
      copy_data("\\N\t");
   }

   sprintf(sql, "SRID=%d;", SRID);
   copy_data(sql);
   copy_data(wkt);

   copy_data("\n");


   return;
}

#if 0
static void add_polygon_error(char osm_type, osmid_t osm_id,
                              const char *key_class, const char *type,
                              struct keyval *names, const char *countrycode,
                              const char *wkt)
{
   int first;
   struct keyval *name;
   char sql[2048];

   /* Output a copy line for this place */
   sprintf(sql, "%c\t%" PRIdOSMID "\t", osm_type, osm_id);
   copy_error_data(sql);

   escape(sql, sizeof(sql), key_class);
   copy_error_data(sql);
   copy_error_data("\t");

   escape(sql, sizeof(sql), type);
   copy_error_data(sql);
   copy_error_data("\t");

   /* start name array */
   if (keyval::listHasData(names))
   {
      first = 1;
      for (name = keyval::firstItem(names); name; name = keyval::nextItem(names, name))
      {
         if (first) first = 0;
         else copy_error_data(", ");

         copy_error_data("\"");

         escape_array_record(sql, sizeof(sql), name->key);
         copy_error_data(sql);

         copy_error_data("\"=>\"");

         escape_array_record(sql, sizeof(sql), name->value);
         copy_error_data(sql);

         copy_error_data("\"");
      }
      copy_error_data("\t");
   }
   else
   {
      copy_error_data("\\N\t");
   }

   if (countrycode)
   {
      escape(sql, sizeof(sql), countrycode);
      copy_error_data(sql);
      copy_error_data("\t");
   }
   else
   {
     copy_error_data("\\N\t");
   }

   copy_error_data("now\tNot a polygon\t\\N\t");

   sprintf(sql, "SRID=%d;", SRID);
   copy_error_data(sql);
   copy_error_data(wkt);

   copy_error_data("\n");


   return;
}
#endif


void output_gazetteer_t::delete_place(char osm_type, osmid_t osm_id)
{
   /* Stop any active copy */
   stop_copy();

   /* Delete all places for this object */
   pgsql_exec(Connection, PGRES_COMMAND_OK, "DELETE FROM place WHERE osm_type = '%c' AND osm_id = %" PRIdOSMID, osm_type, osm_id);

   return;
}

int output_gazetteer_t::connect() {
    /* Connection to the database */
    Connection = PQconnectdb(m_options.conninfo.c_str());

    /* Check to see that the backend connection was successfully made */
    if (PQstatus(Connection) != CONNECTION_OK)
    {
       fprintf(stderr, "Connection to database failed: %s\n", PQerrorMessage(Connection));
       return 1;
    }

    if(m_options.append) {
        ConnectionDelete = PQconnectdb(m_options.conninfo.c_str());
        if (PQstatus(ConnectionDelete) != CONNECTION_OK)
        {
            fprintf(stderr, "Connection to database failed: %s\n", PQerrorMessage(ConnectionDelete));
            return 1;
        }

        pgsql_exec(ConnectionDelete, PGRES_COMMAND_OK, "PREPARE get_classes (CHAR(1), " POSTGRES_OSMID_TYPE ") AS SELECT class FROM place WHERE osm_type = $1 and osm_id = $2");
    }
    return 0;
}

int output_gazetteer_t::start()
{
   reproj = m_options.projection;
   builder.set_exclude_broken_polygon(m_options.excludepoly);

   if(connect())
       util::exit_nicely();

   /* Start a transaction */
   pgsql_exec(Connection, PGRES_COMMAND_OK, "BEGIN");

   /* (Re)create the table unless we are appending */
   if (!m_options.append)
   {
      /* Drop any existing table */
      pgsql_exec(Connection, PGRES_COMMAND_OK, "DROP TABLE IF EXISTS place");
      pgsql_exec(Connection, PGRES_COMMAND_OK, "DROP TYPE if exists keyvalue cascade");
      pgsql_exec(Connection, PGRES_COMMAND_OK, "DROP TYPE if exists wordscore cascade");
      pgsql_exec(Connection, PGRES_COMMAND_OK, "DROP TYPE if exists stringlanguagetype cascade");
      pgsql_exec(Connection, PGRES_COMMAND_OK, "DROP TYPE if exists keyvaluetype cascade");
      pgsql_exec(Connection, PGRES_COMMAND_OK, "DROP FUNCTION IF EXISTS get_connected_ways(integer[])");

      /* Create types and functions */
      pgsql_exec(Connection, PGRES_COMMAND_OK, CREATE_KEYVALUETYPE_TYPE);
      pgsql_exec(Connection, PGRES_COMMAND_OK, CREATE_WORDSCORE_TYPE);

      /* Create the new table */
      if (m_options.tblsmain_data)
      {
          pgsql_exec(Connection, PGRES_COMMAND_OK,
                     CREATE_PLACE_TABLE, "TABLESPACE", m_options.tblsmain_data->c_str());
      }
      else
      {
          pgsql_exec(Connection, PGRES_COMMAND_OK, CREATE_PLACE_TABLE, "", "");
      }
      if (m_options.tblsmain_index)
      {
          pgsql_exec(Connection, PGRES_COMMAND_OK,
                     CREATE_PLACE_ID_INDEX, "TABLESPACE", m_options.tblsmain_index->c_str());
      }
      else
      {
          pgsql_exec(Connection, PGRES_COMMAND_OK, CREATE_PLACE_ID_INDEX, "", "");
      }

      pgsql_exec(Connection, PGRES_TUPLES_OK, "SELECT AddGeometryColumn('place', 'geometry', %d, 'GEOMETRY', 2)", SRID);
      pgsql_exec(Connection, PGRES_COMMAND_OK, "ALTER TABLE place ALTER COLUMN geometry SET NOT NULL");
   }

   return 0;
}

void output_gazetteer_t::commit()
{
}

void output_gazetteer_t::enqueue_ways(pending_queue_t &job_queue, osmid_t id, size_t output_id, size_t& added) {
}

int output_gazetteer_t::pending_way(osmid_t id, int exists) {
    return 0;
}

void output_gazetteer_t::enqueue_relations(pending_queue_t &job_queue, osmid_t id, size_t output_id, size_t& added) {
}

int output_gazetteer_t::pending_relation(osmid_t id, int exists) {
    return 0;
}

void output_gazetteer_t::stop()
{
   /* Stop any active copy */
   stop_copy();

   /* Commit transaction */
   pgsql_exec(Connection, PGRES_COMMAND_OK, "COMMIT");


   PQfinish(Connection);
   if (ConnectionDelete)
       PQfinish(ConnectionDelete);
   if (ConnectionError)
       PQfinish(ConnectionError);

   return;
}

int output_gazetteer_t::gazetteer_process_node(osmid_t id, double lat, double lon, struct keyval *tags, int delete_old)
{
   struct keyval names;
   struct keyval places;
   struct keyval extratags;
   struct keyval *place;
   int adminlevel;
   struct keyval * housenumber;
   struct keyval * street;
   struct keyval * addr_place;
   char * isin;
   struct keyval * postcode;
   struct keyval * countrycode;
   char wkt[128];


   /* Split the tags */
   split_tags(tags, TAGINFO_NODE, &names, &places, &extratags, &adminlevel, &housenumber, &street, &addr_place, &isin, &postcode, &countrycode);

   if (delete_old)
       delete_unused_classes('N', id, &places);

   /* Are we interested in this item? */
   if (keyval::listHasData(&places))
   {
      sprintf(wkt, "POINT(%.15g %.15g)", lon, lat);
      for (place = keyval::firstItem(&places); place; place = keyval::nextItem(&places, place))
      {
         add_place('N', id, place->key, place->value, &names, &extratags, adminlevel, housenumber, street, addr_place, isin, postcode, countrycode, wkt);
      }
   }

   if (housenumber) keyval::freeItem(housenumber);
   if (street) keyval::freeItem(street);
   if (addr_place) keyval::freeItem(addr_place);
   if (isin) free(isin);
   if (postcode) keyval::freeItem(postcode);
   if (countrycode) keyval::freeItem(countrycode);

   /* Free tag lists */
   keyval::resetList(&names);
   keyval::resetList(&places);
   keyval::resetList(&extratags);

   return 0;
}

int output_gazetteer_t::node_add(osmid_t id, double lat, double lon, struct keyval *tags)
{
    return gazetteer_process_node(id, lat, lon, tags, 0);
}

int output_gazetteer_t::gazetteer_process_way(osmid_t id, osmid_t *ndv, int ndc, struct keyval *tags, int delete_old)
{
   struct keyval names;
   struct keyval places;
   struct keyval extratags;
   struct keyval *place;
   int adminlevel;
   struct keyval * housenumber;
   struct keyval * street;
   struct keyval * addr_place;
   char * isin;
   struct keyval * postcode;
   struct keyval * countrycode;
   int area;


   /* Split the tags */
   area = split_tags(tags, TAGINFO_WAY, &names, &places, &extratags, &adminlevel, &housenumber, &street, &addr_place, &isin, &postcode, &countrycode);

   if (delete_old)
       delete_unused_classes('W', id, &places);

   /* Are we interested in this item? */
   if (keyval::listHasData(&places))
   {
      struct osmNode *nodev;
      int nodec;

      /* Fetch the node details */
      nodev = (struct osmNode *)malloc(ndc * sizeof(struct osmNode));
      nodec = m_mid->nodes_get_list(nodev, ndv, ndc);

      /* Get the geometry of the object */
      geometry_builder::maybe_wkt_t wkt = builder.get_wkt_simple(nodev, nodec, area);
      if (wkt)
      {
         for (place = keyval::firstItem(&places); place; place = keyval::nextItem(&places, place))
         {
            add_place('W', id, place->key, place->value, &names, &extratags, adminlevel,
                      housenumber, street, addr_place, isin, postcode, countrycode, wkt->geom.c_str());
         }
      }

      /* Free the nodes */
      free(nodev);
   }

   if (housenumber) keyval::freeItem(housenumber);
   if (street) keyval::freeItem(street);
   if (addr_place) keyval::freeItem(addr_place);
   if (isin) free(isin);
   if (postcode) keyval::freeItem(postcode);
   if (countrycode) keyval::freeItem(countrycode);

   /* Free tag lists */
   keyval::resetList(&names);
   keyval::resetList(&places);
   keyval::resetList(&extratags);

   return 0;
}

int output_gazetteer_t::way_add(osmid_t id, osmid_t *ndv, int ndc, struct keyval *tags)
{
    return gazetteer_process_way(id, ndv, ndc, tags, 0);
}

int output_gazetteer_t::gazetteer_process_relation(osmid_t id, struct member *members, int member_count, struct keyval *tags, int delete_old)
{
   struct keyval names;
   struct keyval places;
   struct keyval extratags;
   struct keyval *place;
   int adminlevel;
   struct keyval * housenumber;
   struct keyval * street;
   struct keyval * addr_place;
   char * isin;
   struct keyval * postcode;
   struct keyval * countrycode;
   const char *type;
   int cmp_waterway;

   type = keyval::getItem(tags, "type");
   if (!type) {
      if (delete_old) delete_unused_classes('R', id, 0);
      return 0;
   }

   cmp_waterway = strcmp(type, "waterway");

   if (!strcmp(type, "associatedStreet"))
   {
      if (delete_old) delete_unused_classes('R', id, 0);
      return 0;
   }

   if (strcmp(type, "boundary") && strcmp(type, "multipolygon") && cmp_waterway) {
      if (delete_old) delete_unused_classes('R', id, 0);
      return 0;
   }

   /* Split the tags */
   split_tags(tags, TAGINFO_AREA, &names, &places, &extratags, &adminlevel, &housenumber, &street, &addr_place, &isin, &postcode, &countrycode);

   /* reset type to NULL because split_tags() consumes the tags
    * keyval and means that it's pointing to some random stuff
    * which might be harmful if dereferenced. */
   type = NULL;

   if (delete_old)
       delete_unused_classes('R', id, &places);

   if (keyval::listHasData(&places))
   {
      /* get the boundary path (ways) */
      int i, count;
      int *xcount = (int *)malloc( (member_count+1) * sizeof(int) );
      keyval *xtags  = new keyval[member_count+1];
      struct osmNode **xnodes = (struct osmNode **)malloc( (member_count+1) * sizeof(struct osmNode*) );
      osmid_t *xid2 = (osmid_t *)malloc( (member_count+1) * sizeof(osmid_t) );

      count = 0;
      for (i=0; i<member_count; i++)
      {
         /* only interested in ways */
         if (members[i].type != OSMTYPE_WAY)
            continue;
         xid2[count] = members[i].id;
         count++;
      }

      if (count == 0)
      {
          if (delete_old) delete_unused_classes('R', id, 0);
          free(xcount);
          delete [] xtags;
          free(xnodes);
          free(xid2);
          return 0;
      }

      osmid_t *xid = (osmid_t *)malloc( sizeof(osmid_t) * (count + 1));
      count = m_mid->ways_get_list(xid2, count, xid, xtags, xnodes, xcount);

      xnodes[count] = NULL;
      xcount[count] = 0;

      if (cmp_waterway)
      {
         geometry_builder::maybe_wkts_t wkts = builder.build_both(xnodes, xcount, 1, 1, 1000000, id);
         for (geometry_builder::wkt_itr wkt = wkts->begin(); wkt != wkts->end(); ++wkt)
         {
            if ((boost::starts_with(wkt->geom,  "POLYGON") || boost::starts_with(wkt->geom,  "MULTIPOLYGON")))
            {
                for (place = keyval::firstItem(&places); place; place = keyval::nextItem(&places, place))
                {
                   add_place('R', id, place->key, place->value, &names, &extratags, adminlevel, housenumber, street, addr_place,
                             isin, postcode, countrycode, wkt->geom.c_str());
                }
            }
            else
            {
                /* add_polygon_error('R', id, "boundary", "adminitrative", &names, countrycode, wkt); */
            }
         }
      } else {
         /* waterways result in multilinestrings */
         // wkt_t build_multilines(const osmNode * const * xnodes, const int *xcount, osmid_t osm_id) const;
         geometry_builder::maybe_wkt_t wkt = builder.build_multilines(xnodes, xcount, id);
         if ((wkt->geom).length() > 0)
         {
            for (place = keyval::firstItem(&places); place; place = keyval::nextItem(&places, place))
            {
               add_place('R', id, place->key, place->value, &names, &extratags, adminlevel, housenumber, street, addr_place,
                         isin, postcode, countrycode, wkt->geom.c_str());
            }
         }
      }
      for( i=0; i<count; i++ )
      {
         keyval::resetList( &(xtags[i]) );
         free( xnodes[i] );
      }

      free(xid);
      free(xid2);
      free(xcount);
      delete [] xtags;
      free(xnodes);
   }

   if (housenumber) keyval::freeItem(housenumber);
   if (street) keyval::freeItem(street);
   if (addr_place) keyval::freeItem(addr_place);
   if (isin) free(isin);
   if (postcode) keyval::freeItem(postcode);
   if (countrycode) keyval::freeItem(countrycode);

   /* Free tag lists */
   keyval::resetList(&names);
   keyval::resetList(&places);
   keyval::resetList(&extratags);

   return 0;
}

int output_gazetteer_t::relation_add(osmid_t id, struct member *members, int member_count, struct keyval *tags)
{
    return gazetteer_process_relation(id, members, member_count, tags, 0);
}

int output_gazetteer_t::node_delete(osmid_t id)
{
   /* Make sure we are in slim mode */
   require_slim_mode();

   /* Delete all references to this node */
   delete_place('N', id);

   return 0;
}

int output_gazetteer_t::way_delete(osmid_t id)
{
   /* Make sure we are in slim mode */
   require_slim_mode();

   /* Delete all references to this way */
   delete_place('W', id);

   return 0;
}

int output_gazetteer_t::relation_delete(osmid_t id)
{
   /* Make sure we are in slim mode */
   require_slim_mode();

   /* Delete all references to this relation */
   delete_place('R', id);

   return 0;
}

int output_gazetteer_t::node_modify(osmid_t id, double lat, double lon, struct keyval *tags)
{
   require_slim_mode();
   return gazetteer_process_node(id, lat, lon, tags, 1);
}

int output_gazetteer_t::way_modify(osmid_t id, osmid_t *ndv, int ndc, struct keyval *tags)
{
   require_slim_mode();
   return gazetteer_process_way(id, ndv, ndc, tags, 1);
}

int output_gazetteer_t::relation_modify(osmid_t id, struct member *members, int member_count, struct keyval *tags)
{
   require_slim_mode();
   return gazetteer_process_relation(id, members, member_count, tags, 1);
}


boost::shared_ptr<output_t> output_gazetteer_t::clone(const middle_query_t* cloned_middle) const {
    output_gazetteer_t *clone = new output_gazetteer_t(*this);
    clone->m_mid = cloned_middle;
    return boost::shared_ptr<output_t>(clone);
}

output_gazetteer_t::output_gazetteer_t(const middle_query_t* mid_, const options_t &options_)
    : output_t(mid_, options_),
      Connection(NULL),
      ConnectionDelete(NULL),
      ConnectionError(NULL),
      CopyActive(0),
      BufferLen(0)
{
    memset(Buffer, 0, BUFFER_SIZE);
}

output_gazetteer_t::output_gazetteer_t(const output_gazetteer_t& other)
    : output_t(other.m_mid, other.m_options),
      Connection(NULL),
      ConnectionDelete(NULL),
      ConnectionError(NULL),
      CopyActive(0),
      BufferLen(0),
      reproj(other.reproj)
{
    builder.set_exclude_broken_polygon(m_options.excludepoly);
    memset(Buffer, 0, BUFFER_SIZE);
    connect();
}

output_gazetteer_t::~output_gazetteer_t() {
}
