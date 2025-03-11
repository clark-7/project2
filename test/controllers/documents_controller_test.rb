require "test_helper"

class DocumentsControllerTest < ActionDispatch::IntegrationTest
  test "should get clear_doc" do
    get documents_clear_doc_url
    assert_response :success
  end
end
