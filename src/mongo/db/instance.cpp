// instance.cpp 

/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "pch.h"
#include "../util/time_support.h"
#include "db.h"
#include "../bson/util/atomic_int.h"
#include "introspect.h"
#include "repl.h"
#include "dbmessage.h"
#include "instance.h"
#include "lasterror.h"
#include "security.h"
#include "json.h"
#include "replutil.h"
#include "../s/d_logic.h"
#include "../util/file_allocator.h"
#include "../util/goodies.h"
#include "cmdline.h"
#if !defined(_WIN32)
#include <sys/file.h>
#endif
#include "stats/counters.h"
#include "background.h"
#include "dur_journal.h"
#include "dur_recover.h"
#include "d_concurrency.h"
#include "ops/count.h"
#include "ops/delete.h"
#include "ops/query.h"
#include "ops/update.h"
#include "pagefault.h"
#include <fstream>
#include <boost/filesystem/operations.hpp>
#include "dur_commitjob.h"

namespace mongo {
    
    // for diaglog
    inline void opread(Message& m) { if( _diaglog.getLevel() & 2 ) _diaglog.readop((char *) m.singleData(), m.header()->len); }
    inline void opwrite(Message& m) { if( _diaglog.getLevel() & 1 ) _diaglog.writeop((char *) m.singleData(), m.header()->len); }

    void receivedKillCursors(Message& m);
    void receivedUpdate(Message& m, CurOp& op);
    void receivedDelete(Message& m, CurOp& op);
    void receivedInsert(Message& m, CurOp& op);
    bool receivedGetMore(DbResponse& dbresponse, Message& m, CurOp& curop );

    int nloggedsome = 0;
#define LOGWITHRATELIMIT if( ++nloggedsome < 1000 || nloggedsome % 100 == 0 )

    string dbExecCommand;

    bool useHints = true;

    KillCurrentOp killCurrentOp;

    int lockFile = 0;
#ifdef _WIN32
    HANDLE lockFileHandle;
#endif

    // see FSyncCommand:
    extern bool lockedForWriting();

    /*static*/ OpTime OpTime::_now() {
        OpTime result;
        unsigned t = (unsigned) time(0);
        if ( last.secs == t ) {
            last.i++;
            result = last;
        }
        else if ( t < last.secs ) {
            result = skewed(); // separate function to keep out of the hot code path
        }
        else { 
            last = OpTime(t, 1);
            result = last;
        }
        notifier.notify_all();
        return last;
    }
    OpTime OpTime::now(const mongo::mutex::scoped_lock&) {
        return _now();
    }
    OpTime OpTime::getLast(const mongo::mutex::scoped_lock&) {
        return last;
    }
    boost::condition OpTime::notifier;
    mongo::mutex OpTime::m("optime");

    // OpTime::now() uses mutex, thus it is in this file not in the cpp files used by drivers and such
    void BSONElementManipulator::initTimestamp() {
        massert( 10332 ,  "Expected CurrentTime type", _element.type() == Timestamp );
        unsigned long long &timestamp = *( reinterpret_cast< unsigned long long* >( value() ) );
        if ( timestamp == 0 ) {
            mutex::scoped_lock lk(OpTime::m);
            timestamp = OpTime::now(lk).asDate();
        }
    }
    void BSONElementManipulator::SetNumber(double d) {
        if ( _element.type() == NumberDouble )
            *getDur().writing( reinterpret_cast< double * >( value() )  ) = d;
        else if ( _element.type() == NumberInt )
            *getDur().writing( reinterpret_cast< int * >( value() ) ) = (int) d;
        else verify(0);
    }
    void BSONElementManipulator::SetLong(long long n) {
        verify( _element.type() == NumberLong );
        *getDur().writing( reinterpret_cast< long long * >(value()) ) = n;
    }
    void BSONElementManipulator::SetInt(int n) {
        verify( _element.type() == NumberInt );
        getDur().writingInt( *reinterpret_cast< int * >( value() ) ) = n;
    }
    /* dur:: version */
    void BSONElementManipulator::ReplaceTypeAndValue( const BSONElement &e ) {
        char *d = data();
        char *v = value();
        int valsize = e.valuesize();
        int ofs = (int) (v-d);
        dassert( ofs > 0 );
        char *p = (char *) getDur().writingPtr(d, valsize + ofs);
        *p = e.type();
        memcpy( p + ofs, e.value(), valsize );
    }

    void inProgCmd( Message &m, DbResponse &dbresponse ) {
        BSONObjBuilder b;

        if( ! cc().isAdmin() ) {
            b.append("err", "unauthorized");
        }
        else {
            DbMessage d(m);
            QueryMessage q(d);
            bool all = q.query["$all"].trueValue();
            vector<BSONObj> vals;
            {
                Client& me = cc();
                scoped_lock bl(Client::clientsMutex);
                scoped_ptr<Matcher> m(new Matcher(q.query));
                for( set<Client*>::iterator i = Client::clients.begin(); i != Client::clients.end(); i++ ) {
                    Client *c = *i;
                    verify( c );
                    CurOp* co = c->curop();
                    if ( c == &me && !co ) {
                        continue;
                    }
                    verify( co );
                    if( all || co->displayInCurop() ) {
                        BSONObj info = co->infoNoauth();
                        if ( all || m->matches( info )) {
                            vals.push_back( info );
                        }
                    }
                }
            }
            b.append("inprog", vals);
            if( lockedForWriting() ) {
                b.append("fsyncLock", true);
                b.append("info", "use db.fsyncUnlock() to terminate the fsync write/snapshot lock");
            }
        }

        replyToQuery(0, m, dbresponse, b.obj());
    }

    void killOp( Message &m, DbResponse &dbresponse ) {
        BSONObj obj;
        if( ! cc().isAdmin() ) {
            obj = fromjson("{\"err\":\"unauthorized\"}");
        }
        /*else if( !dbMutexInfo.isLocked() )
            obj = fromjson("{\"info\":\"no op in progress/not locked\"}");
            */
        else {
            DbMessage d(m);
            QueryMessage q(d);
            BSONElement e = q.query.getField("op");
            if( !e.isNumber() ) {
                obj = fromjson("{\"err\":\"no op number field specified?\"}");
            }
            else {
                log() << "going to kill op: " << e << endl;
                obj = fromjson("{\"info\":\"attempting to kill op\"}");
                killCurrentOp.kill( (unsigned) e.number() );
            }
        }
        replyToQuery(0, m, dbresponse, obj);
    }

    bool _unlockFsync();
    void unlockFsync(const char *ns, Message& m, DbResponse &dbresponse) {
        BSONObj obj;
        if ( ! cc().isAdmin() ) { // checks auth
            obj = fromjson("{\"err\":\"unauthorized\"}");
        }
        else if (strncmp(ns, "admin.", 6) != 0 ) {
            obj = fromjson("{\"err\":\"unauthorized - this command must be run against the admin DB\"}");
        }
        else {
            log() << "command: unlock requested" << endl;
            if( _unlockFsync() ) {
                obj = fromjson("{ok:1,\"info\":\"unlock completed\"}");
            }
            else {
                obj = fromjson("{ok:0,\"errmsg\":\"not locked\"}");
            }
        }
        replyToQuery(0, m, dbresponse, obj);
    }

    static bool receivedQuery(Client& c, DbResponse& dbresponse, Message& m ) {
        bool ok = true;
        MSGID responseTo = m.header()->id;

        DbMessage d(m);
        QueryMessage q(d);
        auto_ptr< Message > resp( new Message() );

        CurOp& op = *(c.curop());

        shared_ptr<AssertionException> ex;

        try {
            dbresponse.exhaust = runQuery(m, q, op, *resp);
            verify( !resp->empty() );
        }
        catch ( SendStaleConfigException& e ){
            ex.reset( new SendStaleConfigException( e.getns(), e.getInfo().msg, e.getVersionReceived(), e.getVersionWanted() ) );
            ok = false;
        }
        catch ( AssertionException& e ) {
            ex.reset( new AssertionException( e.getInfo().msg, e.getCode() ) );
            ok = false;
        }

        if( ex ){

            op.debug().exceptionInfo = ex->getInfo();
            LOGWITHRATELIMIT {
                log() << "assertion " << ex->toString() << " ns:" << q.ns << " query:" <<
                (q.query.valid() ? q.query.toString() : "query object is corrupt") << endl;
                if( q.ntoskip || q.ntoreturn )
                    log() << " ntoskip:" << q.ntoskip << " ntoreturn:" << q.ntoreturn << endl;
            }

            SendStaleConfigException* scex = NULL;
            if ( ex->getCode() == SendStaleConfigCode ) scex = static_cast<SendStaleConfigException*>( ex.get() );

            BSONObjBuilder err;
            ex->getInfo().append( err );
            if( scex ){
                err.append( "ns", scex->getns() );
                err.appendTimestamp( "vReceived", scex->getVersionReceived() );
                err.appendTimestamp( "vWanted", scex->getVersionWanted() );
            }
            BSONObj errObj = err.done();

            log() << errObj << endl;

            BufBuilder b;
            b.skip(sizeof(QueryResult));
            b.appendBuf((void*) errObj.objdata(), errObj.objsize());

            // todo: call replyToQuery() from here instead of this!!! see dbmessage.h
            QueryResult * msgdata = (QueryResult *) b.buf();
            b.decouple();
            QueryResult *qr = msgdata;
            qr->_resultFlags() = ResultFlag_ErrSet;
            if( scex ) qr->_resultFlags() |= ResultFlag_ShardConfigStale;
            qr->len = b.len();
            qr->setOperation(opReply);
            qr->cursorId = 0;
            qr->startingFrom = 0;
            qr->nReturned = 1;
            resp.reset( new Message() );
            resp->setData( msgdata, true );

        }

        op.debug().responseLength = resp->header()->dataLen();

        dbresponse.response = resp.release();
        dbresponse.responseTo = responseTo;

        return ok;
    }

    void (*reportEventToSystem)(const char *msg) = 0;

    void mongoAbort(const char *msg) { 
        if( reportEventToSystem ) 
            reportEventToSystem(msg);
        rawOut(msg);
        ::abort();
    }

    // Returns false when request includes 'end'
    void assembleResponse( Message &m, DbResponse &dbresponse, const HostAndPort& remote ) {

        // before we lock...
        int op = m.operation();
        bool isCommand = false;
        const char *ns = m.singleData()->_data + 4;

        if ( op == dbQuery ) {
            if( strstr(ns, ".$cmd") ) {
                isCommand = true;
                opwrite(m);
                if( strstr(ns, ".$cmd.sys.") ) {
                    if( strstr(ns, "$cmd.sys.inprog") ) {
                        inProgCmd(m, dbresponse);
                        return;
                    }
                    if( strstr(ns, "$cmd.sys.killop") ) {
                        killOp(m, dbresponse);
                        return;
                    }
                    if( strstr(ns, "$cmd.sys.unlock") ) {
                        unlockFsync(ns, m, dbresponse);
                        return;
                    }
                }
            }
            else {
                opread(m);
            }
        }
        else if( op == dbGetMore ) {
            opread(m);
        }
        else {
            opwrite(m);
        }

        globalOpCounters.gotOp( op , isCommand );

        Client& c = cc();
        if ( c.getAuthenticationInfo() )
            c.getAuthenticationInfo()->startRequest();
        
        auto_ptr<CurOp> nestedOp;
        CurOp* currentOpP = c.curop();
        if ( currentOpP->active() ) {
            nestedOp.reset( new CurOp( &c , currentOpP ) );
            currentOpP = nestedOp.get();
        }
        else {
            c.newTopLevelRequest();
        }

        CurOp& currentOp = *currentOpP;
        currentOp.reset(remote,op);

        OpDebug& debug = currentOp.debug();
        debug.op = op;

        int logThreshold = cmdLine.slowMS;
        bool log = logLevel >= 1;

        if ( op == dbQuery ) {
            if ( handlePossibleShardedMessage( m , &dbresponse ) )
                return;
            receivedQuery(c , dbresponse, m );
        }
        else if ( op == dbGetMore ) {
            if ( ! receivedGetMore(dbresponse, m, currentOp) )
                log = true;
        }
        else if ( op == dbMsg ) {
            // deprecated - replaced by commands
            char *p = m.singleData()->_data;
            int len = strlen(p);
            if ( len > 400 )
                out() << curTimeMillis64() % 10000 <<
                      " long msg received, len:" << len << endl;

            Message *resp = new Message();
            if ( strcmp( "end" , p ) == 0 )
                resp->setData( opReply , "dbMsg end no longer supported" );
            else
                resp->setData( opReply , "i am fine - dbMsg deprecated");

            dbresponse.response = resp;
            dbresponse.responseTo = m.header()->id;
        }
        else {
            try {
                // The following operations all require authorization.
                // dbInsert, dbUpdate and dbDelete can be easily pre-authorized,
                // here, but dbKillCursors cannot.
                if ( op == dbKillCursors ) {
                    currentOp.ensureStarted();
                    logThreshold = 10;
                    receivedKillCursors(m);
                }
                else if ( ! c.getAuthenticationInfo()->isAuthorized(
                                  nsToDatabase( m.singleData()->_data + 4 ) ) ) {
                    uassert_nothrow("unauthorized");
                }
                else if ( op == dbInsert ) {
                    receivedInsert(m, currentOp);
                }
                else if ( op == dbUpdate ) {
                    receivedUpdate(m, currentOp);
                }
                else if ( op == dbDelete ) {
                    receivedDelete(m, currentOp);
                }
                else {
                    mongo::log() << "    operation isn't supported: " << op << endl;
                    currentOp.done();
                    log = true;
                }
            }
            catch ( UserException& ue ) {
                tlog(3) << " Caught Assertion in " << opToString(op) << ", continuing "
                        << ue.toString() << endl;
                debug.exceptionInfo = ue.getInfo();
            }
            catch ( AssertionException& e ) {
                tlog(3) << " Caught Assertion in " << opToString(op) << ", continuing "
                        << e.toString() << endl;
                debug.exceptionInfo = e.getInfo();
                log = true;
            }
        }
        currentOp.ensureStarted();
        currentOp.done();
        debug.executionTime = currentOp.totalTimeMillis();

        //DEV log = true;
        if ( log || debug.executionTime > logThreshold ) {
            if( logLevel < 3 && op == dbGetMore && strstr(ns, ".oplog.") && debug.executionTime < 4300 && !log ) {
                /* it's normal for getMore on the oplog to be slow because of use of awaitdata flag. */
            }
            else {
                mongo::tlog() << debug << endl;
            }
        }

        if ( currentOp.shouldDBProfile( debug.executionTime ) ) {
            // performance profiling is on
            if ( Lock::isReadLocked() ) {
                mongo::log(1) << "note: not profiling because recursive read lock" << endl;
            }
            else if ( lockedForWriting() ) {
                mongo::log(1) << "note: not profiling because doing fsync+lock" << endl;
            }
            else {
                writelock lk;
                if ( dbHolder()._isLoaded( nsToDatabase( currentOp.getNS() ) , dbpath ) ) {
                    Client::Context cx( currentOp.getNS(), dbpath, false );
                    profile(c , currentOp );
                }
                else {
                    mongo::log() << "note: not profiling because db went away - probably a close on: " << currentOp.getNS() << endl;
                }
            }
        }
        
        debug.reset();
    } /* assembleResponse() */

    void receivedKillCursors(Message& m) {
        int *x = (int *) m.singleData()->_data;
        x++; // reserved
        int n = *x++;

        uassert( 13659 , "sent 0 cursors to kill" , n != 0 );
        massert( 13658 , str::stream() << "bad kill cursors size: " << m.dataSize() , m.dataSize() == 8 + ( 8 * n ) );
        uassert( 13004 , str::stream() << "sent negative cursors to kill: " << n  , n >= 1 );

        if ( n > 2000 ) {
            log( n < 30000 ? LL_WARNING : LL_ERROR ) << "receivedKillCursors, n=" << n << endl;
            verify( n < 30000 );
        }

        int found = ClientCursor::erase(n, (long long *) x);

        if ( logLevel > 0 || found != n ) {
            log( found == n ) << "killcursors: found " << found << " of " << n << endl;
        }

    }

    /* db - database name
       path - db directory
    */
    /*static*/ void Database::closeDatabase( const char *db, const string& path ) {
        assertInWriteLock();

        Client::Context * ctx = cc().getContext();
        verify( ctx );
        verify( ctx->inDB( db , path ) );
        Database *database = ctx->db();
        verify( database->name == db );

        oplogCheckCloseDatabase( database ); // oplog caches some things, dirty its caches

        if( BackgroundOperation::inProgForDb(db) ) {
            log() << "warning: bg op in prog during close db? " << db << endl;
        }

        /* important: kill all open cursors on the database */
        string prefix(db);
        prefix += '.';
        ClientCursor::invalidate(prefix.c_str());

        NamespaceDetailsTransient::clearForPrefix( prefix.c_str() );

        dbHolderW().erase( db, path );
        ctx->_clear();
        delete database; // closes files
    }

    void receivedUpdate(Message& m, CurOp& op) {
        DbMessage d(m);
        const char *ns = d.getns();
        op.debug().ns = ns;
        int flags = d.pullInt();
        BSONObj query = d.nextJsObj();

        verify( d.moreJSObjs() );
        verify( query.objsize() < m.header()->dataLen() );
        BSONObj toupdate = d.nextJsObj();
        uassert( 10055 , "update object too large", toupdate.objsize() <= BSONObjMaxUserSize);
        verify( toupdate.objsize() < m.header()->dataLen() );
        verify( query.objsize() + toupdate.objsize() < m.header()->dataLen() );
        bool upsert = flags & UpdateOption_Upsert;
        bool multi = flags & UpdateOption_Multi;
        bool broadcast = flags & UpdateOption_Broadcast;
        
        op.debug().query = query;
        op.setQuery(query);
        
        PageFaultRetryableSection s;
        while ( 1 ) {
            try {
                Lock::DBWrite lk(ns);
                
                // void ReplSetImpl::relinquish() uses big write lock so 
                // this is thus synchronized given our lock above.
                uassert( 10054 ,  "not master", isMasterNs( ns ) );
                
                // if this ever moves to outside of lock, need to adjust check Client::Context::_finishInit
                if ( ! broadcast && handlePossibleShardedMessage( m , 0 ) )
                    return;
                
                Client::Context ctx( ns );
                
                UpdateResult res = updateObjects(ns, toupdate, query, upsert, multi, true, op.debug() );
                lastError.getSafe()->recordUpdate( res.existing , res.num , res.upserted ); // for getlasterror
                break;
            }
            catch ( PageFaultException& e ) {
                e.touch();
            }
        }
    }

    void receivedDelete(Message& m, CurOp& op) {
        DbMessage d(m);
        const char *ns = d.getns();
        op.debug().ns = ns;
        int flags = d.pullInt();
        bool justOne = flags & RemoveOption_JustOne;
        bool broadcast = flags & RemoveOption_Broadcast;
        verify( d.moreJSObjs() );
        BSONObj pattern = d.nextJsObj();
        
        op.debug().query = pattern;
        op.setQuery(pattern);

        writelock lk(ns);

        // writelock is used to synchronize stepdowns w/ writes
        uassert( 10056 ,  "not master", isMasterNs( ns ) );

        // if this ever moves to outside of lock, need to adjust check Client::Context::_finishInit
        if ( ! broadcast && handlePossibleShardedMessage( m , 0 ) )
            return;

        Client::Context ctx(ns);

        long long n = deleteObjects(ns, pattern, justOne, true);
        lastError.getSafe()->recordDelete( n );
    }

    QueryResult* emptyMoreResult(long long);

    void OpTime::waitForDifferent(unsigned millis){
        mutex::scoped_lock lk(m);
        while (*this == last) {
            if (!notifier.timed_wait(lk.boost(), boost::posix_time::milliseconds(millis)))
                return; // timed out
        }
    }

    bool receivedGetMore(DbResponse& dbresponse, Message& m, CurOp& curop ) {
        bool ok = true;

        DbMessage d(m);

        const char *ns = d.getns();
        int ntoreturn = d.pullInt();
        long long cursorid = d.pullInt64();

        curop.debug().ns = ns;
        curop.debug().ntoreturn = ntoreturn;
        curop.debug().cursorid = cursorid;

        shared_ptr<AssertionException> ex;
        time_t start = 0;
        int pass = 0;
        bool exhaust = false;
        QueryResult* msgdata = 0;
        OpTime last;
        while( 1 ) {
            try {
                if (str::startsWith(ns, "local.oplog.")){
                    if (pass == 0) {
                        mutex::scoped_lock lk(OpTime::m);
                        last = OpTime::getLast(lk);
                    }
                    else {
                        last.waitForDifferent(1000/*ms*/);
                    }
                }

                Client::ReadContext ctx(ns);

                // call this readlocked so state can't change
                replVerifyReadsOk();
                msgdata = processGetMore(ns, ntoreturn, cursorid, curop, pass, exhaust);
            }
            catch ( AssertionException& e ) {
                ex.reset( new AssertionException( e.getInfo().msg, e.getCode() ) );
                ok = false;
                break;
            }

            if (msgdata == 0) {
                exhaust = false;
                massert(13073, "shutting down", !inShutdown() );
                if( pass == 0 ) {
                    start = time(0);
                }
                else {
                    if( time(0) - start >= 4 ) {
                        // after about 4 seconds, return. pass stops at 1000 normally.
                        // we want to return occasionally so slave can checkpoint.
                        pass = 10000;
                    }
                }
                pass++;
                if (debug)
                    sleepmillis(20);
                else
                    sleepmillis(2);
                continue;
            }
            break;
        };

        if (ex) {
            exhaust = false;

            BSONObjBuilder err;
            ex->getInfo().append( err );
            BSONObj errObj = err.done();

            log() << errObj << endl;

            curop.debug().exceptionInfo = ex->getInfo();

            if (ex->getCode() == 13436) {
                replyToQuery(ResultFlag_ErrSet, m, dbresponse, errObj);
                curop.debug().responseLength = dbresponse.response->header()->dataLen();
                curop.debug().nreturned = 1;
                return ok;
            }

            msgdata = emptyMoreResult(cursorid);
        }

        Message *resp = new Message();
        resp->setData(msgdata, true);
        curop.debug().responseLength = resp->header()->dataLen();
        curop.debug().nreturned = msgdata->nReturned;

        dbresponse.response = resp;
        dbresponse.responseTo = m.header()->id;
        
        if( exhaust ) {
            curop.debug().exhaust = true;
            dbresponse.exhaust = ns;
        }

        return ok;
    }

    void checkAndInsert(const char *ns, /*modifies*/BSONObj& js) { 
        uassert( 10059 , "object to insert too large", js.objsize() <= BSONObjMaxUserSize);
        {
            // check no $ modifiers.  note we only check top level.  (scanning deep would be quite expensive)
            BSONObjIterator i( js );
            while ( i.more() ) {
                BSONElement e = i.next();
                uassert( 13511 , "document to insert can't have $ fields" , e.fieldName()[0] != '$' );
            }
        }
        theDataFileMgr.insertWithObjMod(ns, js, false); // js may be modified in the call to add an _id field.
        logOp("i", ns, js);
    }

    NOINLINE_DECL void insertMulti(bool keepGoing, const char *ns, vector<BSONObj>& objs) {
        size_t i;
        for (i=0; i<objs.size(); i++){
            try {
                checkAndInsert(ns, objs[i]);
                getDur().commitIfNeeded();
            } catch (const UserException&) {
                if (!keepGoing || i == objs.size()-1){
                    globalOpCounters.incInsertInWriteLock(i);
                    throw;
                }
                // otherwise ignore and keep going
            }
        }

        globalOpCounters.incInsertInWriteLock(i);
    }

    void receivedInsert(Message& m, CurOp& op) {
        DbMessage d(m);
        const char *ns = d.getns();
        op.debug().ns = ns;

        if( !d.moreJSObjs() ) {
            // strange.  should we complain?
            return;
        }
        BSONObj first = d.nextJsObj();

        vector<BSONObj> multi;
        while (d.moreJSObjs()){
            if (multi.empty()) // first pass
                multi.push_back(first);
            multi.push_back( d.nextJsObj() );
        }

        writelock lk(ns);

        // CONCURRENCY TODO: is being read locked in big log sufficient here?
        // writelock is used to synchronize stepdowns w/ writes
        uassert( 10058 , "not master", isMasterNs(ns) );

        if ( handlePossibleShardedMessage( m , 0 ) )
            return;

        Client::Context ctx(ns);

        if( !multi.empty() ) {
            const bool keepGoing = d.reservedField() & InsertOption_ContinueOnError;
            insertMulti(keepGoing, ns, multi);
            return;
        }

        checkAndInsert(ns, first);
        globalOpCounters.incInsertInWriteLock(1);
    }

    void getDatabaseNames( vector< string > &names , const string& usePath ) {
        boost::filesystem::path path( usePath );
        for ( boost::filesystem::directory_iterator i( path );
                i != boost::filesystem::directory_iterator(); ++i ) {
            if ( directoryperdb ) {
                boost::filesystem::path p = *i;
                string dbName = p.leaf();
                p /= ( dbName + ".ns" );
                if ( exists( p ) )
                    names.push_back( dbName );
            }
            else {
                string fileName = boost::filesystem::path(*i).leaf();
                if ( fileName.length() > 3 && fileName.substr( fileName.length() - 3, 3 ) == ".ns" )
                    names.push_back( fileName.substr( 0, fileName.length() - 3 ) );
            }
        }
    }

    /* returns true if there is data on this server.  useful when starting replication.
       local database does NOT count except for rsoplog collection.
       used to set the hasData field on replset heartbeat command response
    */
    bool replHasDatabases() {
        vector<string> names;
        getDatabaseNames(names);
        if( names.size() >= 2 ) return true;
        if( names.size() == 1 ) {
            if( names[0] != "local" )
                return true;
            // we have a local database.  return true if oplog isn't empty
            {
                readlock lk(rsoplog);
                BSONObj o;
                if( Helpers::getFirst(rsoplog, o) )
                    return true;
            }
        }
        return false;
    }

    QueryOptions DBDirectClient::_lookupAvailableOptions() {
        // Exhaust mode is not available in DBDirectClient.
        return QueryOptions(DBClientBase::_lookupAvailableOptions() & ~QueryOption_Exhaust);
    }

    bool DBDirectClient::call( Message &toSend, Message &response, bool assertOk , string * actualServer ) {
        if ( lastError._get() )
            lastError.startRequest( toSend, lastError._get() );
        DbResponse dbResponse;
        assembleResponse( toSend, dbResponse , _clientHost );
        verify( dbResponse.response );
        dbResponse.response->concat(); // can get rid of this if we make response handling smarter
        response = *dbResponse.response;
        getDur().commitIfNeeded();
        return true;
    }

    void DBDirectClient::say( Message &toSend, bool isRetry, string * actualServer ) {
        if ( lastError._get() )
            lastError.startRequest( toSend, lastError._get() );
        DbResponse dbResponse;
        assembleResponse( toSend, dbResponse , _clientHost );
        getDur().commitIfNeeded();
    }

    auto_ptr<DBClientCursor> DBDirectClient::query(const string &ns, Query query, int nToReturn , int nToSkip ,
            const BSONObj *fieldsToReturn , int queryOptions , int batchSize) {

        //if ( ! query.obj.isEmpty() || nToReturn != 0 || nToSkip != 0 || fieldsToReturn || queryOptions )
        return DBClientBase::query( ns , query , nToReturn , nToSkip , fieldsToReturn , queryOptions , batchSize );
        //
        //verify( query.obj.isEmpty() );
        //throw UserException( (string)"yay:" + ns );
    }

    void DBDirectClient::killCursor( long long id ) {
        ClientCursor::erase( id );
    }

    HostAndPort DBDirectClient::_clientHost = HostAndPort( "0.0.0.0" , 0 );

    unsigned long long DBDirectClient::count(const string &ns, const BSONObj& query, int options, int limit, int skip ) {
        Lock::DBRead lk( ns );
        string errmsg;
        long long res = runCount( ns.c_str() , _countCmd( ns , query , options , limit , skip ) , errmsg );
        if ( res == -1 )
            return 0;
        uassert( 13637 , str::stream() << "count failed in DBDirectClient: " << errmsg , res >= 0 );
        return (unsigned long long )res;
    }

    DBClientBase * createDirectClient() {
        return new DBDirectClient();
    }

    mongo::mutex exitMutex("exit");
    AtomicUInt numExitCalls = 0;

    bool inShutdown() {
        return numExitCalls > 0;
    }

    void tryToOutputFatal( const string& s ) {
        try {
            rawOut( s );
            return;
        }
        catch ( ... ) {}

        try {
            cerr << s << endl;
            return;
        }
        catch ( ... ) {}

        // uh - oh, not sure there is anything else we can do...
    }

    /** also called by ntservice.cpp */
    void shutdownServer() {

        log() << "shutdown: going to close listening sockets..." << endl;
        ListeningSockets::get()->closeAll();

        log() << "shutdown: going to flush diaglog..." << endl;
        _diaglog.flush();

        /* must do this before unmapping mem or you may get a seg fault */
        log() << "shutdown: going to close sockets..." << endl;
        boost::thread close_socket_thread( boost::bind(MessagingPort::closeAllSockets, 0) );

        // wait until file preallocation finishes
        // we would only hang here if the file_allocator code generates a
        // synchronous signal, which we don't expect
        log() << "shutdown: waiting for fs preallocator..." << endl;
        FileAllocator::get()->waitUntilFinished();

        if( cmdLine.dur ) {
            log() << "shutdown: lock for final commit..." << endl;
            {
                int n = 10;
                while( 1 ) {
                    // we may already be in a read lock from earlier in the call stack, so do read lock here 
                    // to be consistent with that.
                    readlocktry w(20000);
                    if( w.got() ) { 
                        log() << "shutdown: final commit..." << endl;
                        getDur().commitNow();
                        break;
                    }
                    if( --n <= 0 ) {
                        log() << "shutdown: couldn't acquire write lock, aborting" << endl;
                        mongoAbort("couldn't acquire write lock");
                    }
                    log() << "shutdown: waiting for write lock..." << endl;
                }
            }
            MemoryMappedFile::flushAll(true);
        }

        log() << "shutdown: closing all files..." << endl;
        stringstream ss3;
        MemoryMappedFile::closeAllFiles( ss3 );
        log() << ss3.str() << endl;

        if( cmdLine.dur ) {
            dur::journalCleanup(true);
        }

#if !defined(__sunos__)
        if ( lockFile ) {
            log() << "shutdown: removing fs lock..." << endl;
            /* This ought to be an unlink(), but Eliot says the last
               time that was attempted, there was a race condition
               with acquirePathLock().  */
#ifdef _WIN32
            if( _chsize( lockFile , 0 ) )
                log() << "couldn't remove fs lock " << WSAGetLastError() << endl;
            CloseHandle(lockFileHandle);
#else
            if( ftruncate( lockFile , 0 ) )
                log() << "couldn't remove fs lock " << errnoWithDescription() << endl;
            flock( lockFile, LOCK_UN );
#endif
        }
#endif
    }

    void exitCleanly( ExitCode code ) {
        killCurrentOp.killAll();
        {
            Lock::GlobalWrite lk;
            log() << "now exiting" << endl;
            dbexit( code );
        }
    }

    /* not using log() herein in case we are already locked */
    NOINLINE_DECL void dbexit( ExitCode rc, const char *why, bool tryToGetLock ) {

        auto_ptr<writelocktry> wlt;
        if ( tryToGetLock ) {
            wlt.reset( new writelocktry( 2 * 60 * 1000 ) );
            uassert( 13455 , "dbexit timed out getting lock" , wlt->got() );
        }

        Client * c = currentClient.get();
        {
            scoped_lock lk( exitMutex );
            if ( numExitCalls++ > 0 ) {
                if ( numExitCalls > 5 ) {
                    // this means something horrible has happened
                    ::_exit( rc );
                }
                stringstream ss;
                ss << "dbexit: " << why << "; exiting immediately";
                tryToOutputFatal( ss.str() );
                if ( c ) c->shutdown();
                ::exit( rc );
            }
        }

        {
            stringstream ss;
            ss << "dbexit: " << why;
            tryToOutputFatal( ss.str() );
        }

        try {
            shutdownServer(); // gracefully shutdown instance
        }
        catch ( ... ) {
            tryToOutputFatal( "shutdown failed with exception" );
        }

#if defined(_DEBUG)
        try {
            mutexDebugger.programEnding();
        }
        catch (...) { }
#endif

        // block the dur thread from doing any work for the rest of the run
        log(2) << "shutdown: groupCommitMutex" << endl;
        SimpleMutex::scoped_lock lk(dur::commitJob.groupCommitMutex);

#ifdef _WIN32
        // Windows Service Controller wants to be told when we are down,
        //  so don't call ::exit() yet, or say "really exiting now"
        //
        if ( rc == EXIT_WINDOWS_SERVICE_STOP ) {
            if ( c ) c->shutdown();
            return;
        }
#endif
        tryToOutputFatal( "dbexit: really exiting now" );
        if ( c ) c->shutdown();
        ::exit(rc);
    }

#if !defined(__sunos__)
    void writePid(int fd) {
        stringstream ss;
        ss << getpid() << endl;
        string s = ss.str();
        const char * data = s.c_str();
#ifdef _WIN32
        verify( _write( fd, data, strlen( data ) ) );
#else
        verify( write( fd, data, strlen( data ) ) );
#endif
    }

    void acquirePathLock(bool doingRepair) {
        string name = ( boost::filesystem::path( dbpath ) / "mongod.lock" ).native_file_string();

        bool oldFile = false;

        if ( boost::filesystem::exists( name ) && boost::filesystem::file_size( name ) > 0 ) {
            oldFile = true;
        }

#ifdef _WIN32
        lockFileHandle = CreateFileA( name.c_str(), GENERIC_READ | GENERIC_WRITE,
            0 /* do not allow anyone else access */, NULL, 
            OPEN_ALWAYS /* success if fh can open */, 0, NULL );

        if (lockFileHandle == INVALID_HANDLE_VALUE) {
            DWORD code = GetLastError();
            char *msg;
            FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
                NULL, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                (LPSTR)&msg, 0, NULL);
            string m = msg;
            str::stripTrailing(m, "\r\n");
            uasserted( 13627 , str::stream() << "Unable to create/open lock file: " << name << ' ' << m << ". Is a mongod instance already running?" );
        }
        lockFile = _open_osfhandle((intptr_t)lockFileHandle, 0);
#else
        lockFile = open( name.c_str(), O_RDWR | O_CREAT , S_IRWXU | S_IRWXG | S_IRWXO );
        if( lockFile <= 0 ) {
            uasserted( 10309 , str::stream() << "Unable to create/open lock file: " << name << ' ' << errnoWithDescription() << " Is a mongod instance already running?" );
        }
        if (flock( lockFile, LOCK_EX | LOCK_NB ) != 0) {
            close ( lockFile );
            lockFile = 0;
            uassert( 10310 ,  "Unable to lock file: " + name + ". Is a mongod instance already running?",  0 );
        }
#endif

        if ( oldFile ) {
            // we check this here because we want to see if we can get the lock
            // if we can't, then its probably just another mongod running
            
            string errmsg;
            if (cmdLine.dur) {
                if (!dur::haveJournalFiles()) {
                    
                    vector<string> dbnames;
                    getDatabaseNames( dbnames );
                    
                    if ( dbnames.size() == 0 ) {
                        // this means that mongod crashed
                        // between initial startup and when journaling was initialized
                        // it is safe to continue
                    }
                    else {
                        errmsg = str::stream()
                            << "************** \n"
                            << "old lock file: " << name << ".  probably means unclean shutdown,\n"
                            << "but there are no journal files to recover.\n"
                            << "this is likely human error or filesystem corruption.\n"
                            << "found " << dbnames.size() << " dbs.\n"
                            << "see: http://dochub.mongodb.org/core/repair for more information\n"
                            << "*************";
                    }


                }
            }
            else {
                if (!dur::haveJournalFiles() && !doingRepair) {
                    errmsg = str::stream()
                             << "************** \n"
                             << "Unclean shutdown detected.\n"
                             << "Please visit http://dochub.mongodb.org/core/repair for recovery instructions.\n"
                             << "*************";
                }
            }

            if (!errmsg.empty()) {
                cout << errmsg << endl;
#ifdef _WIN32
                CloseHandle( lockFileHandle );
#else
                close ( lockFile );
#endif
                lockFile = 0;
                uassert( 12596 , "old lock file" , 0 );
            }
        }

        // Not related to lock file, but this is where we handle unclean shutdown
        if( !cmdLine.dur && dur::haveJournalFiles() ) {
            cout << "**************" << endl;
            cout << "Error: journal files are present in journal directory, yet starting without journaling enabled." << endl;
            cout << "It is recommended that you start with journaling enabled so that recovery may occur." << endl;
            cout << "**************" << endl;
            uasserted(13597, "can't start without --journal enabled when journal/ files are present");
        }

#ifdef _WIN32
        uassert( 13625, "Unable to truncate lock file", _chsize(lockFile, 0) == 0);
        writePid( lockFile );
        _commit( lockFile );
#else
        uassert( 13342, "Unable to truncate lock file", ftruncate(lockFile, 0) == 0);
        writePid( lockFile );
        fsync( lockFile );
        flushMyDirectory(name);
#endif
    }
#else
    void acquirePathLock(bool) {
        // TODO - this is very bad that the code above not running here.

        // Not related to lock file, but this is where we handle unclean shutdown
        if( !cmdLine.dur && dur::haveJournalFiles() ) {
            cout << "**************" << endl;
            cout << "Error: journal files are present in journal directory, yet starting without --journal enabled." << endl;
            cout << "It is recommended that you start with journaling enabled so that recovery may occur." << endl;
            cout << "Alternatively (not recommended), you can backup everything, then delete the journal files, and run --repair" << endl;
            cout << "**************" << endl;
            uasserted(13618, "can't start without --journal enabled when journal/ files are present");
        }
    }
#endif

    // ----- BEGIN Diaglog -----
    DiagLog::DiagLog() : f(0) , level(0), mutex("DiagLog") { 
    }

    void DiagLog::openFile() {
        verify( f == 0 );
        stringstream ss;
        ss << dbpath << "/diaglog." << hex << time(0);
        string name = ss.str();
        f = new ofstream(name.c_str(), ios::out | ios::binary);
        if ( ! f->good() ) {
            problem() << "diagLogging couldn't open " << name << endl;
            // todo what is this? :
            throw 1717;
        }
        else {
            log() << "diagLogging using file " << name << endl;
        }
    }

    int DiagLog::setLevel( int newLevel ) {
        scoped_lock lk(mutex);
        int old = level;
        log() << "diagLogging level=" << newLevel << endl;
        if( f == 0 ) { 
            openFile();
        }
        level = newLevel; // must be done AFTER f is set
        return old;
    }
    
    void DiagLog::flush() {
        if ( level ) {
            log() << "flushing diag log" << endl;
            scoped_lock lk(mutex);
            f->flush();
        }
    }
    
    void DiagLog::writeop(char *data,int len) {
        if ( level & 1 ) {
            scoped_lock lk(mutex);
            f->write(data,len);
        }
    }
    
    void DiagLog::readop(char *data, int len) {
        if ( level & 2 ) {
            bool log = (level & 4) == 0;
            OCCASIONALLY log = true;
            if ( log ) {
                scoped_lock lk(mutex);
                verify( f );
                f->write(data,len);
            }
        }
    }

    DiagLog _diaglog;

    // ----- END Diaglog -----

} // namespace mongo
