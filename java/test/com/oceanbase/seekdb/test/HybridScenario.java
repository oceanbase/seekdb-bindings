package com.oceanbase.seekdb.test;

import java.sql.*;
import java.util.*;

/** Shared JDBC equivalent of pyseekdb/examples/hybrid_search_example.py scenario 1.
 * Uses explicit deterministic embeddings so testing needs no model download. */
final class HybridScenario {
    static String run(Connection c) throws Exception {
        String[] documents = {
            "Machine learning is revolutionizing artificial intelligence and data science",
            "Python programming language is essential for machine learning developers",
            "Deep learning neural networks enable advanced AI applications",
            "Data science combines statistics, programming, and domain expertise",
            "Natural language processing uses machine learning to understand text",
            "Computer vision algorithms process images using deep learning techniques",
            "Reinforcement learning trains agents through reward-based feedback",
            "Python libraries like TensorFlow and PyTorch simplify machine learning",
            "Artificial intelligence systems can learn from large datasets",
            "Neural networks mimic the structure of biological brain connections"
        };
        try (Statement s = c.createStatement()) {
            s.execute("DROP TABLE IF EXISTS android_hybrid_demo");
            s.execute("CREATE TABLE android_hybrid_demo (_id VARCHAR(64) PRIMARY KEY, document STRING, embedding VECTOR(3), metadata JSON, FULLTEXT INDEX idx_fts(document), VECTOR INDEX idx_vec(embedding) WITH(distance=l2,type=hnsw,lib=vsag)) ORGANIZATION=HEAP");
            try (PreparedStatement p = c.prepareStatement("INSERT INTO android_hybrid_demo VALUES (?, ?, ?, ?)")) {
                for (int i = 0; i < documents.length; i++) {
                    p.setString(1, "doc_" + (i + 1)); p.setString(2, documents[i]);
                    p.setString(3, "[" + (i * 0.1) + ",0.2,0.3]");
                    p.setString(4, "{\"year\":2023}"); p.executeUpdate();
                }
            }
            s.execute("CALL dbms_index_manager.refresh()");
            try (ResultSet r = s.executeQuery("SHOW PARAMETERS LIKE 'mysql_port_mode'")) {
                if (!r.next() || !"disabled".equals(r.getString("value")))
                    throw new AssertionError("TCP must be disabled");
            }
            try (ResultSet r = s.executeQuery("SELECT COUNT(*) FROM android_hybrid_demo WHERE MATCH(document) AGAINST('machine learning' IN NATURAL LANGUAGE MODE)")) {
                if (!r.next() || r.getInt(1) < 4) throw new AssertionError("Fulltext search failed");
            }
            String params = "{\"query\":{\"query_string\":{\"fields\":[\"document\"],\"query\":\"machine learning\"}},\"knn\":{\"field\":\"embedding\",\"k\":10,\"query_vector\":[0,0.2,0.3]},\"rank\":{\"rrf\":{}},\"size\":5,\"_source\":[\"_id\",\"document\",\"metadata\"]}";
            String sql;
            try (PreparedStatement p = c.prepareStatement("SELECT DBMS_HYBRID_SEARCH.GET_SQL('android_hybrid_demo', ?)")) {
                p.setString(1, params);
                try (ResultSet r = p.executeQuery()) {
                    if (!r.next()) throw new AssertionError("GET_SQL returned no SQL");
                    sql = r.getString(1);
                }
            }
            System.out.println("HYBRID_SQL=" + sql);
            List<String> ids = new ArrayList<>();
            try (ResultSet r = s.executeQuery(sql)) {
                while (r.next()) ids.add(r.getString("_id"));
            }
            if (ids.size() != 5 || new HashSet<>(ids).size() != 5 || !ids.contains("doc_1"))
                throw new AssertionError("Unexpected hybrid results " + ids);
            return "HYBRID_OK rows=10 top5=" + ids + " tcp=disabled";
        }
    }
}
